#include "deevee_core.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DEEVEE_MIN_MENU_VIDEO_PAYLOAD_BYTES 65536u
#define DEEVEE_MENU_VISIBILITY_SAMPLE_FRAMES 30u
#define DEEVEE_DEFAULT_MENU_FRAME_REPEAT 2u
#define DEEVEE_CLOCK_TICKS_PER_SECOND 90000u
#define DEEVEE_CLOCK_TICKS_PER_RUN 1500u
#define DEEVEE_MIN_FRAME_DURATION_TICKS 750u
#define DEEVEE_MAX_FRAME_DURATION_TICKS 15000u

struct payload_extract_context
{
   struct deevee_core *core;
   bool ok;
};

struct packet_extract_context
{
   struct deevee_core *core;
   bool ok;
};

static bool decode_next_menu_frame(struct deevee_core *core);
static bool fill_frame_queue(struct deevee_core *core, size_t target_count);
static void decode_audio_until_buffered(struct deevee_core *core,
      size_t target_frames);
static bool open_menu_decoder(struct deevee_core *core);
static bool prepare_title_playback_from_command(struct deevee_core *core,
      const uint8_t command[8]);

static bool content_is_disc_image(const struct deevee_content_info *content)
{
   return content && (content->type == DEEVEE_CONTENT_ISO ||
         content->type == DEEVEE_CONTENT_CHD);
}

static uint32_t deevee_core_rgb(unsigned r, unsigned g, unsigned b)
{
   return 0xff000000u | ((r & 0xffu) << 16) |
      ((g & 0xffu) << 8) | (b & 0xffu);
}

static unsigned fallback_frame_duration_ticks(const struct deevee_core *core)
{
   if (core && core->video_frame_duration_ticks)
      return core->video_frame_duration_ticks;

   return DEEVEE_DEFAULT_MENU_FRAME_REPEAT * DEEVEE_CLOCK_TICKS_PER_RUN;
}

static unsigned clamp_frame_duration_ticks(uint64_t ticks)
{
   if (ticks < DEEVEE_MIN_FRAME_DURATION_TICKS)
      return DEEVEE_MIN_FRAME_DURATION_TICKS;
   if (ticks > DEEVEE_MAX_FRAME_DURATION_TICKS)
      return DEEVEE_MAX_FRAME_DURATION_TICKS;
   return (unsigned)ticks;
}

static unsigned display_frame_duration_ticks(const struct deevee_core *core,
      uint64_t ticks)
{
   unsigned fallback = fallback_frame_duration_ticks(core);
   unsigned duration;

   if (!ticks)
      return fallback;

   duration = clamp_frame_duration_ticks(ticks);

   if (duration < fallback / 2u || duration > fallback * 2u)
      return fallback;

   return duration;
}

static unsigned display_hold_ticks_from_duration(unsigned duration)
{
   unsigned display_runs;

   if (!duration)
      return 0;

   display_runs = (duration + (DEEVEE_CLOCK_TICKS_PER_RUN / 2u)) /
      DEEVEE_CLOCK_TICKS_PER_RUN;
   if (display_runs < 1u)
      display_runs = 1u;

   return (display_runs - 1u) * DEEVEE_CLOCK_TICKS_PER_RUN;
}

static unsigned frame_duration_ticks_from_frame_rate_code(uint8_t code)
{
   switch (code)
   {
      case 1: /* 24000 / 1001 */
         return 3754u;
      case 2: /* 24 */
         return 3750u;
      case 3: /* 25 */
         return 3600u;
      case 4: /* 30000 / 1001 */
         return 3003u;
      case 5: /* 30 */
         return 3000u;
      case 6: /* 50 */
         return 1800u;
      case 7: /* 60000 / 1001 */
         return 1502u;
      case 8: /* 60 */
         return 1500u;
      default:
         return DEEVEE_DEFAULT_MENU_FRAME_REPEAT * DEEVEE_CLOCK_TICKS_PER_RUN;
   }
}

static size_t frame_queue_index(size_t head, size_t offset)
{
   return (head + offset) % DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY;
}

static void reset_frame_queue(struct deevee_core *core)
{
   size_t i;

   if (!core)
      return;

   for (i = 0; i < DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY; i++)
   {
      if (core->frame_queue[i].pixels)
         memset(core->frame_queue[i].pixels, 0,
               DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT * sizeof(uint32_t));
      core->frame_queue[i].valid = false;
      core->frame_queue[i].has_pts = false;
      core->frame_queue[i].pts = 0;
      core->frame_queue[i].display_ticks = fallback_frame_duration_ticks(core);
      core->frame_queue[i].width = 0;
      core->frame_queue[i].height = 0;
      core->frame_queue[i].pixel_format = 0;
   }

   core->frame_queue_head = 0;
   core->frame_queue_count = 0;
   core->frame_clock_pts = 0;
   core->frame_clock_has_pts = false;
   core->display_frame_ticks_remaining = 0;
   core->displayed_frames = 0;
   core->repeated_frames = 0;
   core->decode_underruns = 0;
   core->queue_drops = 0;
   core->fallback_timing_frames = 0;
}

static bool allocate_frame_queue(struct deevee_core *core)
{
   size_t i;

   if (!core)
      return false;

   if (!core->decoder_output_pixels)
   {
      core->decoder_output_pixels = (uint32_t *)calloc(
            DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT, sizeof(uint32_t));
      if (!core->decoder_output_pixels)
         return false;
   }

   for (i = 0; i < DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY; i++)
   {
      if (!core->frame_queue[i].pixels)
      {
         core->frame_queue[i].pixels = (uint32_t *)calloc(
               DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT, sizeof(uint32_t));
         if (!core->frame_queue[i].pixels)
            return false;
      }
   }

   reset_frame_queue(core);
   return true;
}

static void free_frame_queue(struct deevee_core *core)
{
   size_t i;

   if (!core)
      return;

   free(core->decoder_output_pixels);
   core->decoder_output_pixels = NULL;
   for (i = 0; i < DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY; i++)
   {
      free(core->frame_queue[i].pixels);
      memset(&core->frame_queue[i], 0, sizeof(core->frame_queue[i]));
   }

   core->frame_queue_head = 0;
   core->frame_queue_count = 0;
}

static void clear_menu_video(struct deevee_core *core)
{
   if (!core)
      return;

   deevee_decoder_deinit(&core->decoder);
   free(core->menu_video_payloads);
   free(core->menu_video_chunks);
   free(core->audio_payloads);
   free(core->audio_chunks);
   core->menu_video_payloads = NULL;
   core->menu_video_payload_size = 0;
   core->menu_video_payload_capacity = 0;
   core->menu_video_chunks = NULL;
   core->menu_video_chunk_count = 0;
   core->menu_video_chunk_capacity = 0;
   core->next_menu_video_chunk = 0;
   core->audio_payloads = NULL;
   core->audio_payload_size = 0;
   core->audio_payload_capacity = 0;
   core->audio_chunks = NULL;
   core->audio_chunk_count = 0;
   core->audio_chunk_capacity = 0;
   core->next_audio_chunk = 0;
   core->audio_payload_packets = 0;
   deevee_audio_deinit(&core->audio);
   deevee_audio_init(&core->audio);
   core->menu_frame_hold = 0;
   core->menu_frame_repeat = DEEVEE_DEFAULT_MENU_FRAME_REPEAT;
   core->video_frame_rate_code = 0;
   core->video_frame_duration_ticks =
      frame_duration_ticks_from_frame_rate_code(0);
   core->menu_playback_active = false;
   core->menu_playback_source[0] = '\0';
   core->menu_packets_sent = 0;
   core->menu_frames_decoded = 0;
   core->menu_last_frame_width = 0;
   core->menu_last_frame_height = 0;
   core->menu_last_pixel_format = 0;
   core->menu_button_count = 0;
   core->menu_active_button = 0;
   core->menu_confirmed_button = 0;
   core->menu_last_nav_mask = 0;
   memset(core->menu_buttons, 0, sizeof(core->menu_buttons));
   core->menu_post_command_count = 0;
   memset(core->menu_post_commands, 0, sizeof(core->menu_post_commands));
   memset(core->menu_resolved_jump_command, 0,
         sizeof(core->menu_resolved_jump_command));
   core->menu_has_resolved_jump = false;
   core->menu_current_vts = 0;
   core->playback_is_title = false;
   core->menu_last_frame_has_pts = false;
   core->menu_last_frame_pts = 0;
   core->menu_previous_frame_has_pts = false;
   core->menu_previous_frame_pts = 0;
   reset_frame_queue(core);
}

static bool append_menu_video_payload(struct deevee_core *core,
      const uint8_t *payload, size_t payload_size)
{
   uint8_t *new_payloads;
   struct deevee_video_payload_chunk *new_chunks;
   size_t new_payload_capacity;
   size_t new_chunk_capacity;

   if (!core || !payload || !payload_size)
      return false;

   if (core->menu_video_payload_size + payload_size >
         core->menu_video_payload_capacity)
   {
      new_payload_capacity = core->menu_video_payload_capacity ?
         core->menu_video_payload_capacity * 2u : 4096u;
      while (new_payload_capacity <
            core->menu_video_payload_size + payload_size)
         new_payload_capacity *= 2u;

      new_payloads = (uint8_t *)realloc(core->menu_video_payloads,
            new_payload_capacity);
      if (!new_payloads)
         return false;

      core->menu_video_payloads = new_payloads;
      core->menu_video_payload_capacity = new_payload_capacity;
   }

   if (core->menu_video_chunk_count + 1u > core->menu_video_chunk_capacity)
   {
      new_chunk_capacity = core->menu_video_chunk_capacity ?
         core->menu_video_chunk_capacity * 2u : 16u;
      new_chunks = (struct deevee_video_payload_chunk *)realloc(
            core->menu_video_chunks,
            new_chunk_capacity * sizeof(*core->menu_video_chunks));
      if (!new_chunks)
         return false;

      core->menu_video_chunks = new_chunks;
      core->menu_video_chunk_capacity = new_chunk_capacity;
   }

   memcpy(core->menu_video_payloads + core->menu_video_payload_size,
         payload, payload_size);
   core->menu_video_chunks[core->menu_video_chunk_count].offset =
      core->menu_video_payload_size;
   core->menu_video_chunks[core->menu_video_chunk_count].size = payload_size;
   core->menu_video_chunks[core->menu_video_chunk_count].has_pts = false;
   core->menu_video_chunks[core->menu_video_chunk_count].pts = 0;
   core->menu_video_chunks[core->menu_video_chunk_count].has_dts = false;
   core->menu_video_chunks[core->menu_video_chunk_count].dts = 0;
   core->menu_video_payload_size += payload_size;
   core->menu_video_chunk_count++;
   return true;
}

static bool append_menu_video_packet(struct deevee_core *core,
      const struct deevee_dvd_packet *packet)
{
   if (!core || !packet || !packet->payload || !packet->payload_size)
      return false;

   if (!append_menu_video_payload(core, packet->payload, packet->payload_size))
      return false;

   core->menu_video_chunks[core->menu_video_chunk_count - 1u].has_pts =
      packet->has_pts;
   core->menu_video_chunks[core->menu_video_chunk_count - 1u].pts =
      packet->pts;
   core->menu_video_chunks[core->menu_video_chunk_count - 1u].has_dts =
      packet->has_dts;
   core->menu_video_chunks[core->menu_video_chunk_count - 1u].dts =
      packet->dts;
   return true;
}

static bool append_audio_payload(struct deevee_core *core,
      const uint8_t *payload, size_t payload_size)
{
   uint8_t *new_payloads;
   struct deevee_audio_payload_chunk *new_chunks;
   size_t new_payload_capacity;
   size_t new_chunk_capacity;

   if (!core || !payload || !payload_size)
      return false;

   if (core->audio_payload_size + payload_size >
         core->audio_payload_capacity)
   {
      new_payload_capacity = core->audio_payload_capacity ?
         core->audio_payload_capacity * 2u : 4096u;
      while (new_payload_capacity < core->audio_payload_size + payload_size)
         new_payload_capacity *= 2u;

      new_payloads = (uint8_t *)realloc(core->audio_payloads,
            new_payload_capacity);
      if (!new_payloads)
         return false;

      core->audio_payloads = new_payloads;
      core->audio_payload_capacity = new_payload_capacity;
   }

   if (core->audio_chunk_count + 1u > core->audio_chunk_capacity)
   {
      new_chunk_capacity = core->audio_chunk_capacity ?
         core->audio_chunk_capacity * 2u : 16u;
      new_chunks = (struct deevee_audio_payload_chunk *)realloc(
            core->audio_chunks, new_chunk_capacity *
            sizeof(*core->audio_chunks));
      if (!new_chunks)
         return false;

      core->audio_chunks = new_chunks;
      core->audio_chunk_capacity = new_chunk_capacity;
   }

   memcpy(core->audio_payloads + core->audio_payload_size, payload,
         payload_size);
   core->audio_chunks[core->audio_chunk_count].offset =
      core->audio_payload_size;
   core->audio_chunks[core->audio_chunk_count].size = payload_size;
   core->audio_payload_size += payload_size;
   core->audio_chunk_count++;
   core->audio_payload_packets++;
   return true;
}

static bool append_ac3_packet(struct deevee_core *core,
      const struct deevee_dvd_packet *packet)
{
   const uint8_t *payload;

   if (!core || !packet || packet->stream_id != 0xbd ||
         !packet->payload || packet->payload_size <= 4u)
      return true;

   payload = packet->payload;
   if (payload[0] < 0x80 || payload[0] > 0x87)
      return true;

   return append_audio_payload(core, payload + 4u, packet->payload_size - 4u);
}

static unsigned menu_repeat_from_frame_rate_code(uint8_t frame_rate_code)
{
   switch (frame_rate_code)
   {
      case 1: /* 24000 / 1001 */
      case 2: /* 24 */
         return 3;
      case 3: /* 25 */
      case 4: /* 30000 / 1001 */
      case 5: /* 30 */
         return 2;
      case 6: /* 50 */
      case 7: /* 60000 / 1001 */
      case 8: /* 60 */
         return 1;
      default:
         return DEEVEE_DEFAULT_MENU_FRAME_REPEAT;
   }
}

static void detect_menu_frame_repeat(struct deevee_core *core)
{
   size_t chunk_index;

   if (!core)
      return;

   core->menu_frame_repeat = DEEVEE_DEFAULT_MENU_FRAME_REPEAT;
   core->video_frame_rate_code = 0;
   core->video_frame_duration_ticks =
      frame_duration_ticks_from_frame_rate_code(0);

   for (chunk_index = 0; chunk_index < core->menu_video_chunk_count;
         chunk_index++)
   {
      const struct deevee_video_payload_chunk *chunk =
         &core->menu_video_chunks[chunk_index];
      const uint8_t *payload = core->menu_video_payloads + chunk->offset;
      size_t i;

      for (i = 0; i + 12 <= chunk->size; i++)
      {
         if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
               payload[i + 2] == 0x01 && payload[i + 3] == 0xb3)
         {
            core->video_frame_rate_code = payload[i + 7] & 0x0fu;
            core->video_frame_duration_ticks =
               frame_duration_ticks_from_frame_rate_code(
                  core->video_frame_rate_code);
            core->menu_frame_repeat =
               menu_repeat_from_frame_rate_code(payload[i + 7] & 0x0fu);
            return;
         }
      }
   }
}

static bool reset_menu_decoder_position(struct deevee_core *core)
{
   if (!core || !core->menu_video_chunk_count)
      return false;

   if (!open_menu_decoder(core))
      return false;

   reset_frame_queue(core);
   core->next_menu_video_chunk = 0;
   core->menu_frame_hold = 0;
   core->menu_packets_sent = 0;
   core->menu_frames_decoded = 0;
   core->menu_last_frame_width = 0;
   core->menu_last_frame_height = 0;
   core->menu_last_pixel_format = 0;
   core->menu_last_frame_has_pts = false;
   core->menu_last_frame_pts = 0;
   core->menu_previous_frame_has_pts = false;
   core->menu_previous_frame_pts = 0;
   core->video_frame_duration_ticks =
      frame_duration_ticks_from_frame_rate_code(core->video_frame_rate_code);
   return true;
}

static void set_menu_buttons_from_probe(struct deevee_core *core,
      const struct deevee_dvd_menu_render_probe *probe)
{
   uint8_t initial_button;

   if (!core || !probe || !probe->button_count)
      return;

   core->menu_button_count = probe->button_count;
   memcpy(core->menu_buttons, probe->buttons,
         (size_t)probe->button_count * sizeof(core->menu_buttons[0]));
   core->menu_post_command_count = probe->parsed_post_command_count;
   memcpy(core->menu_post_commands, probe->post_commands,
         (size_t)probe->parsed_post_command_count *
         sizeof(core->menu_post_commands[0]));

   initial_button = probe->forced_select_button;
   if (!initial_button)
      initial_button = probe->starting_button;
   if (!initial_button || initial_button > probe->button_count)
      initial_button = 1;

   core->menu_active_button = initial_button;
}

static bool is_link_tail_pgc_command(const uint8_t command[8])
{
   return command && command[0] == 0x20 && (command[1] & 0x0fu) == 0x01 &&
      (command[7] & 0x1fu) == 0x0d;
}

static bool is_direct_jump_command(const uint8_t command[8])
{
   return command && command[0] == 0x30 &&
      ((command[1] & 0x0fu) == 0x02 ||
       (command[1] & 0x0fu) == 0x03 ||
       (command[1] & 0x0fu) == 0x05 ||
       (command[1] & 0x0fu) == 0x06);
}

static void resolve_menu_button_command(struct deevee_core *core,
      const uint8_t command[8])
{
   uint8_t command_index;

   if (!core || !command)
      return;

   memset(core->menu_resolved_jump_command, 0,
         sizeof(core->menu_resolved_jump_command));
   core->menu_has_resolved_jump = false;

   if (is_direct_jump_command(command))
   {
      memcpy(core->menu_resolved_jump_command, command,
            sizeof(core->menu_resolved_jump_command));
      core->menu_has_resolved_jump = true;
      return;
   }

   if (!is_link_tail_pgc_command(command))
      return;

   for (command_index = 0; command_index < core->menu_post_command_count;
         command_index++)
   {
      if (is_direct_jump_command(core->menu_post_commands[command_index]))
      {
         memcpy(core->menu_resolved_jump_command,
               core->menu_post_commands[command_index],
               sizeof(core->menu_resolved_jump_command));
         core->menu_has_resolved_jump = true;
         return;
      }
   }
}

static void update_menu_button_selection(struct deevee_core *core)
{
   uint32_t nav_mask;
   uint32_t pressed;
   const struct deevee_dvd_menu_button *button;
   uint8_t next_button = 0;

   if (!core || !core->menu_button_count || !core->menu_active_button ||
         core->menu_active_button > core->menu_button_count)
      return;

   nav_mask = deevee_nav_active_mask(&core->nav);
   pressed = nav_mask & ~core->menu_last_nav_mask;
   core->menu_last_nav_mask = nav_mask;

   button = &core->menu_buttons[core->menu_active_button - 1u];
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_UP))
      next_button = button->up;
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_DOWN))
      next_button = button->down;
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_LEFT))
      next_button = button->left;
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_RIGHT))
      next_button = button->right;
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_CONFIRM))
   {
      core->menu_confirmed_button = core->menu_active_button;
      resolve_menu_button_command(core, button->command);
      if (core->menu_has_resolved_jump)
         (void)prepare_title_playback_from_command(core,
               core->menu_resolved_jump_command);
   }

   if (next_button && next_button <= core->menu_button_count)
      core->menu_active_button = next_button;
}

static void draw_menu_button_overlay(struct deevee_core *core)
{
   const struct deevee_dvd_menu_button *button;
   uint32_t color;
   uint16_t x0;
   uint16_t x1;
   uint16_t y0;
   uint16_t y1;
   unsigned x;
   unsigned y;

   if (!core || !core->video.pixels || !core->menu_button_count ||
         !core->menu_active_button ||
         core->menu_active_button > core->menu_button_count)
      return;

   button = &core->menu_buttons[core->menu_active_button - 1u];
   x0 = button->x_start < DEEVEE_VIDEO_WIDTH ? button->x_start :
      DEEVEE_VIDEO_WIDTH - 1u;
   x1 = button->x_end < DEEVEE_VIDEO_WIDTH ? button->x_end :
      DEEVEE_VIDEO_WIDTH - 1u;
   y0 = button->y_start < DEEVEE_VIDEO_HEIGHT ? button->y_start :
      DEEVEE_VIDEO_HEIGHT - 1u;
   y1 = button->y_end < DEEVEE_VIDEO_HEIGHT ? button->y_end :
      DEEVEE_VIDEO_HEIGHT - 1u;

   if (x1 < x0 || y1 < y0)
      return;

   color = core->menu_confirmed_button == core->menu_active_button ?
      deevee_core_rgb(255, 192, 32) : deevee_core_rgb(32, 255, 96);

   for (y = y0; y <= y1; y++)
   {
      for (x = x0; x <= x1; x++)
      {
         bool border = x < x0 + 4u || x + 4u > x1 ||
            y < y0 + 4u || y + 4u > y1;

         if (border || ((x + y + (unsigned)core->frame_count) % 17u) == 0)
            core->video.pixels[y * DEEVEE_VIDEO_WIDTH + x] = color;
      }
   }
}

static bool should_draw_menu_overlay(const struct deevee_core *core)
{
   return core && !core->playback_is_title;
}

static void enqueue_decoded_frame(struct deevee_video_decoder *decoder,
      const struct deevee_decoder_frame_probe *frame, void *user_data)
{
   struct deevee_core *core = (struct deevee_core *)user_data;
   struct deevee_decoded_video_frame *queued;
   size_t queue_index;
   unsigned fallback_ticks;

   if (!decoder || !frame || !core || !decoder->output_pixels)
      return;

   if (core->frame_queue_count >= DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY)
   {
      core->frame_queue_head = frame_queue_index(core->frame_queue_head, 1u);
      core->frame_queue_count--;
      core->queue_drops++;
   }

   fallback_ticks = fallback_frame_duration_ticks(core);
   if (core->frame_queue_count)
   {
      struct deevee_decoded_video_frame *previous =
         &core->frame_queue[frame_queue_index(core->frame_queue_head,
               core->frame_queue_count - 1u)];

      if (previous->valid && previous->has_pts && frame->has_pts &&
            frame->pts > previous->pts)
         previous->display_ticks = display_frame_duration_ticks(core,
               (uint64_t)(frame->pts - previous->pts));
      else if (previous->valid)
      {
         previous->display_ticks = fallback_frame_duration_ticks(core);
         core->fallback_timing_frames++;
      }
   }

   queue_index = frame_queue_index(core->frame_queue_head,
         core->frame_queue_count);
   queued = &core->frame_queue[queue_index];
   memcpy(queued->pixels, decoder->output_pixels,
         DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT * sizeof(uint32_t));
   queued->valid = true;
   queued->has_pts = frame->has_pts;
   queued->pts = frame->pts;
   queued->display_ticks = fallback_ticks;
   queued->width = frame->width;
   queued->height = frame->height;
   queued->pixel_format = frame->pixel_format;

   if (!frame->has_pts)
      core->fallback_timing_frames++;

   core->frame_queue_count++;
}

static bool display_next_queued_frame(struct deevee_core *core)
{
   struct deevee_decoded_video_frame *queued;

   if (!core || !core->frame_queue_count)
      return false;

   queued = &core->frame_queue[core->frame_queue_head];
   if (!queued->valid || !queued->pixels)
      return false;

   memcpy(core->video.pixels, queued->pixels,
         DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT * sizeof(uint32_t));
   core->menu_last_frame_width = queued->width;
   core->menu_last_frame_height = queued->height;
   core->menu_last_pixel_format = queued->pixel_format;
   core->menu_previous_frame_has_pts = core->menu_last_frame_has_pts;
   core->menu_previous_frame_pts = core->menu_last_frame_pts;
   core->menu_last_frame_has_pts = queued->has_pts;
   core->menu_last_frame_pts = queued->pts;
   core->display_frame_ticks_remaining =
      display_hold_ticks_from_duration(queued->display_ticks);
   core->displayed_frames++;

   if (queued->has_pts)
   {
      core->frame_clock_pts = queued->pts;
      core->frame_clock_has_pts = true;
   }
   else
   {
      core->frame_clock_pts += fallback_frame_duration_ticks(core);
      core->frame_clock_has_pts = false;
   }

   queued->valid = false;
   core->frame_queue_head = frame_queue_index(core->frame_queue_head, 1u);
   core->frame_queue_count--;
   return true;
}

static bool extract_payload_callback(const uint8_t *payload,
      size_t payload_size, void *user_data)
{
   struct payload_extract_context *context =
      (struct payload_extract_context *)user_data;

   context->ok = append_menu_video_payload(context->core, payload,
         payload_size);
   return context->ok;
}

static bool extract_video_packet_callback(
      const struct deevee_dvd_packet *packet, void *user_data)
{
   struct packet_extract_context *context =
      (struct packet_extract_context *)user_data;

   if (!context || !packet)
      return false;

   if (!append_ac3_packet(context->core, packet))
   {
      context->ok = false;
      return false;
   }

   if (packet->stream_id < 0xe0 || packet->stream_id > 0xef)
      return true;

   context->ok = append_menu_video_packet(context->core, packet);
   return context->ok;
}

static bool open_menu_decoder(struct deevee_core *core)
{
   if (!core)
      return false;

   deevee_decoder_deinit(&core->decoder);
   if (deevee_decoder_init(&core->decoder) != DEEVEE_DECODER_OK)
      return false;
   if (deevee_decoder_open_mpeg2(&core->decoder) != DEEVEE_DECODER_OK)
      return false;
   if (deevee_decoder_set_xrgb8888_output(&core->decoder,
            core->decoder_output_pixels, DEEVEE_VIDEO_WIDTH, DEEVEE_VIDEO_HEIGHT,
            core->video.pitch) != DEEVEE_DECODER_OK)
      return false;
   deevee_decoder_set_frame_callback(&core->decoder, enqueue_decoded_frame,
         core);

   return true;
}

static bool prepare_menu_playback(struct deevee_core *core,
      const struct deevee_content_info *content)
{
   struct deevee_disc disc;
   struct deevee_dvd_info dvd_info;
   struct payload_extract_context extract_context;
   bool prepared = false;

   if (!core || !content_is_disc_image(content))
      return false;

   clear_menu_video(core);
   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, content) != DEEVEE_DISC_OK)
      return false;

   memset(&dvd_info, 0, sizeof(dvd_info));
   if (deevee_dvd_probe(&disc, &dvd_info) != DEEVEE_DVD_OK)
      goto end_disc;

   extract_context.core = core;
   extract_context.ok = true;
   if (deevee_dvd_walk_first_menu_video_payloads(&disc, &dvd_info,
            extract_payload_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   prepared = true;

end_disc:
   deevee_disc_close(&disc);
   if (!prepared)
      clear_menu_video(core);
   else
   {
      strncpy(core->menu_playback_source, "first-menu-pgc",
            sizeof(core->menu_playback_source) - 1);
      core->menu_playback_source[sizeof(core->menu_playback_source) - 1] =
         '\0';
   }
   return prepared;
}

static bool prepare_menu_playback_from_vob_path(struct deevee_core *core,
      const struct deevee_content_info *content, const char *iso_path)
{
   struct deevee_disc disc;
   struct payload_extract_context extract_context;
   bool prepared = false;

   if (!core || !content_is_disc_image(content) || !iso_path)
      return false;

   clear_menu_video(core);
   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, content) != DEEVEE_DISC_OK)
      return false;

   extract_context.core = core;
   extract_context.ok = true;
   if (deevee_dvd_walk_vob_video_payloads(&disc, iso_path,
            extract_payload_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   prepared = true;

end_disc:
   deevee_disc_close(&disc);
   if (!prepared)
      clear_menu_video(core);
   else
   {
      strncpy(core->menu_playback_source, iso_path,
            sizeof(core->menu_playback_source) - 1);
      core->menu_playback_source[sizeof(core->menu_playback_source) - 1] =
         '\0';
   }
   return prepared;
}

static bool prepare_menu_playback_from_vts_pgc(struct deevee_core *core,
      const struct deevee_content_info *content, unsigned vts,
      unsigned pgc_index)
{
   struct deevee_disc disc;
   struct payload_extract_context extract_context;
   struct deevee_dvd_menu_render_probe render_probe;
   bool prepared = false;

   if (!core || !content_is_disc_image(content))
      return false;

   clear_menu_video(core);
   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, content) != DEEVEE_DISC_OK)
      return false;

   extract_context.core = core;
   extract_context.ok = true;
   if (deevee_dvd_walk_vts_menu_pgc_video_payloads(&disc, vts, pgc_index,
            extract_payload_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   if (deevee_dvd_probe_vts_menu_pgc_render_streams(&disc, vts, pgc_index,
            &render_probe) == DEEVEE_DVD_OK)
      set_menu_buttons_from_probe(core, &render_probe);

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   prepared = true;

end_disc:
   deevee_disc_close(&disc);
   if (!prepared)
      clear_menu_video(core);
   else
   {
      snprintf(core->menu_playback_source,
            sizeof(core->menu_playback_source), "VTS_%02u_0.PGC_%02u",
            vts, pgc_index + 1u);
      core->menu_playback_source[sizeof(core->menu_playback_source) - 1] =
         '\0';
      core->menu_current_vts = (uint8_t)vts;
   }
   return prepared;
}

static bool prepare_title_playback_from_command(struct deevee_core *core,
      const uint8_t command[8])
{
   uint8_t command_copy[8];
   uint8_t current_vts;
   struct deevee_disc disc;
   struct deevee_dvd_info dvd_info;
   struct deevee_dvd_title_table title_table;
   struct deevee_dvd_playback_target target;
   struct deevee_dvd_title_pgc title_pgc;
   struct packet_extract_context extract_context;
   bool prepared = false;
   bool cleared_video = false;

   if (!core || !command || !content_is_disc_image(&core->content))
      return false;

   memcpy(command_copy, command, sizeof(command_copy));
   current_vts = core->menu_current_vts;
   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, &core->content) != DEEVEE_DISC_OK)
      return false;

   memset(&dvd_info, 0, sizeof(dvd_info));
   memset(&title_table, 0, sizeof(title_table));
   memset(&target, 0, sizeof(target));
   memset(&title_pgc, 0, sizeof(title_pgc));

   if (deevee_dvd_probe(&disc, &dvd_info) != DEEVEE_DVD_OK ||
         deevee_dvd_read_title_table(&disc, &dvd_info, &title_table) !=
            DEEVEE_DVD_OK ||
         !deevee_dvd_decode_playback_target_command(command_copy, current_vts,
            &title_table, &target) ||
         deevee_dvd_resolve_title_pgc(&disc, &target, &title_pgc) !=
            DEEVEE_DVD_OK)
      goto end_disc;

   clear_menu_video(core);
   cleared_video = true;
   extract_context.core = core;
   extract_context.ok = true;
   if (deevee_dvd_walk_title_pgc_packets(&disc, &title_pgc,
            extract_video_packet_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   core->menu_playback_active = true;
   core->playback_is_title = true;
   core->menu_current_vts = title_pgc.vts_number;
   snprintf(core->menu_playback_source, sizeof(core->menu_playback_source),
         "VTS_%02u_TITLE_%02u_PGC_%02u", title_pgc.vts_number,
         title_pgc.vts_title_number, title_pgc.pgc_number);
   core->menu_playback_source[sizeof(core->menu_playback_source) - 1] = '\0';
   decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 6u);
   prepared = true;

end_disc:
   deevee_disc_close(&disc);
   if (!prepared && cleared_video)
      clear_menu_video(core);
   return prepared;
}

static bool keep_prepared_menu_if_decodable(struct deevee_core *core)
{
   unsigned sample_frame;

   if (!core)
      return false;

   if (core->menu_video_payload_size < DEEVEE_MIN_MENU_VIDEO_PAYLOAD_BYTES)
   {
      clear_menu_video(core);
      return false;
   }

   core->menu_playback_active = true;
   for (sample_frame = 0; sample_frame < DEEVEE_MENU_VISIBILITY_SAMPLE_FRAMES;
         sample_frame++)
   {
      uint32_t max_component = 0;
      size_t visible_pixels = 0;
      size_t i;

      if (!decode_next_menu_frame(core))
         break;

      for (i = 0; i < DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT; i++)
      {
         uint32_t pixel = core->video.pixels[i];
         uint32_t r = (pixel >> 16) & 0xffu;
         uint32_t g = (pixel >> 8) & 0xffu;
         uint32_t b = pixel & 0xffu;
         uint32_t component = r;

         if (g > component)
            component = g;
         if (b > component)
            component = b;
         if (component > max_component)
            max_component = component;
         if (component > 24u)
            visible_pixels++;
      }

      if (max_component > 32u && visible_pixels > 1000u)
         return reset_menu_decoder_position(core);
   }

   clear_menu_video(core);
   return false;
}

static bool prepare_any_menu_playback(struct deevee_core *core,
      const struct deevee_content_info *content)
{
   unsigned vts;
   char path[32];

   if (prepare_menu_playback(core, content) &&
         keep_prepared_menu_if_decodable(core))
      return true;

   for (vts = 1; vts <= 99; vts++)
   {
      unsigned pgc_index;

      for (pgc_index = 0; pgc_index < 32; pgc_index++)
      {
         if (prepare_menu_playback_from_vts_pgc(core, content, vts,
                  pgc_index) && keep_prepared_menu_if_decodable(core))
            return true;
      }
   }

   if (prepare_menu_playback_from_vob_path(core, content,
            "/VIDEO_TS/VIDEO_TS.VOB") &&
         keep_prepared_menu_if_decodable(core))
      return true;

   for (vts = 1; vts <= 99; vts++)
   {
      snprintf(path, sizeof(path), "/VIDEO_TS/VTS_%02u_0.VOB", vts);
      if (prepare_menu_playback_from_vob_path(core, content, path) &&
            keep_prepared_menu_if_decodable(core))
         return true;
   }

   return false;
}

static bool fill_frame_queue(struct deevee_core *core, size_t target_count)
{
   struct deevee_decoder_frame_probe frame_probe;
   size_t attempts = 0;

   if (!core || !core->menu_video_chunk_count)
      return false;

   memset(&frame_probe, 0, sizeof(frame_probe));

   while (core->frame_queue_count < target_count &&
         attempts < core->menu_video_chunk_count * 2u)
   {
      const struct deevee_video_payload_chunk *chunk;
      const uint8_t *payload;
      uint32_t packets_before = frame_probe.packets_sent;
      uint32_t frames_before = frame_probe.frames_decoded;

      if (core->next_menu_video_chunk >= core->menu_video_chunk_count)
      {
         uint32_t frames_before_flush = frame_probe.frames_decoded;

         (void)deevee_decoder_flush_mpeg2(&core->decoder, &frame_probe);
         core->menu_frames_decoded +=
            frame_probe.frames_decoded - frames_before_flush;
         if (core->frame_queue_count >= target_count)
            break;
         if (!open_menu_decoder(core))
            return false;
         core->next_menu_video_chunk = 0;
      }

      chunk = &core->menu_video_chunks[core->next_menu_video_chunk++];
      payload = core->menu_video_payloads + chunk->offset;
      if (deevee_decoder_decode_mpeg2_timed_payload(&core->decoder, payload,
               chunk->size, chunk->has_pts, chunk->pts, chunk->has_dts,
               chunk->dts, &frame_probe) != DEEVEE_DECODER_OK)
      {
         if (core->playback_is_title)
         {
            attempts++;
            continue;
         }
         if (!open_menu_decoder(core))
            return false;
         attempts++;
         continue;
      }

      core->menu_packets_sent += frame_probe.packets_sent - packets_before;
      core->menu_frames_decoded += frame_probe.frames_decoded - frames_before;

      attempts++;
   }

   return core->frame_queue_count > 0;
}

static bool decode_next_menu_frame(struct deevee_core *core)
{
   size_t target_count;

   if (!core)
      return false;

   target_count = core->playback_is_title ? 4u : 2u;
   if (core->frame_queue_count < target_count)
      (void)fill_frame_queue(core, target_count);

   if (display_next_queued_frame(core))
      return true;

   core->decode_underruns++;
   return fill_frame_queue(core, 1u) && display_next_queued_frame(core);
}

static void decode_audio_until_buffered(struct deevee_core *core,
      size_t target_frames)
{
   size_t attempts = 0;

   if (!core || !core->audio_chunk_count)
      return;

   while (deevee_audio_buffered_frames(&core->audio) < target_frames &&
         attempts < core->audio_chunk_count)
   {
      const struct deevee_audio_payload_chunk *chunk;
      enum deevee_audio_status status;

      if (core->next_audio_chunk >= core->audio_chunk_count)
      {
         deevee_audio_deinit(&core->audio);
         deevee_audio_init(&core->audio);
         core->next_audio_chunk = 0;
      }

      chunk = &core->audio_chunks[core->next_audio_chunk++];
      status = deevee_audio_decode_ac3_payload(&core->audio,
            core->audio_payloads + chunk->offset, chunk->size);
      if (status != DEEVEE_AUDIO_OK)
         core->audio.decode_errors++;

      attempts++;
   }
}

bool deevee_core_init(struct deevee_core *core)
{
   if (!core)
      return false;

   memset(core, 0, sizeof(*core));
   deevee_nav_init(&core->nav);
   deevee_audio_init(&core->audio);

   if (!deevee_video_init(&core->video))
      return false;
   if (!allocate_frame_queue(core))
   {
      free_frame_queue(core);
      deevee_video_deinit(&core->video);
      return false;
   }

   core->initialized = true;
   return true;
}

void deevee_core_deinit(struct deevee_core *core)
{
   if (!core)
      return;

   clear_menu_video(core);
   free_frame_queue(core);
   deevee_video_deinit(&core->video);
   memset(core, 0, sizeof(*core));
}

bool deevee_core_load(struct deevee_core *core, const char *path)
{
   struct deevee_content_info content;

   if (!core || !core->initialized)
      return false;

   if (!deevee_content_probe(path, &content))
      return false;

   core->content = content;
   core->loaded = true;
   core->menu_playback_active = prepare_any_menu_playback(core, &content);
   core->frame_count = 0;
   deevee_nav_init(&core->nav);
   return true;
}

void deevee_core_unload(struct deevee_core *core)
{
   if (!core)
      return;

   clear_menu_video(core);
   core->loaded = false;
   core->frame_count = 0;
   memset(&core->content, 0, sizeof(core->content));
   deevee_nav_init(&core->nav);
}

void deevee_core_reset(struct deevee_core *core)
{
   if (!core)
      return;

   core->frame_count = 0;
   deevee_nav_init(&core->nav);
}

void deevee_core_set_button(struct deevee_core *core,
      enum deevee_nav_button button, bool pressed)
{
   if (!core)
      return;

   deevee_nav_set_button(&core->nav, button, pressed);
}

void deevee_core_run(struct deevee_core *core, struct deevee_frame *video,
      struct deevee_audio_frame *audio)
{
   const char *label = "no content";

   if (!core || !video || !audio)
      return;

   if (core->loaded)
      label = deevee_content_type_name(core->content.type);

   update_menu_button_selection(core);

   if (core->menu_playback_active)
   {
      if (core->display_frame_ticks_remaining)
      {
         if (core->display_frame_ticks_remaining > DEEVEE_CLOCK_TICKS_PER_RUN)
            core->display_frame_ticks_remaining -= DEEVEE_CLOCK_TICKS_PER_RUN;
         else
            core->display_frame_ticks_remaining = 0;
         core->repeated_frames++;
         if (core->frame_queue_count < (core->playback_is_title ? 4u : 2u))
            (void)fill_frame_queue(core, core->playback_is_title ? 4u : 2u);
      }
      else if (!decode_next_menu_frame(core))
      {
         core->menu_playback_active = false;
         deevee_video_render_placeholder(&core->video, core->frame_count,
               label);
      }
   }
   else
      deevee_video_render_placeholder(&core->video, core->frame_count, label);

   if (should_draw_menu_overlay(core))
      draw_menu_button_overlay(core);

   video->pixels = core->video.pixels;
   video->width = DEEVEE_VIDEO_WIDTH;
   video->height = DEEVEE_VIDEO_HEIGHT;
   video->pitch = core->video.pitch;
   decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 3u);
   audio->samples = deevee_audio_read(&core->audio, &audio->frames);

   core->frame_count++;
}

const char *deevee_core_loaded_content_type(const struct deevee_core *core)
{
   if (!core || !core->loaded)
      return deevee_content_type_name(DEEVEE_CONTENT_NONE);

   return deevee_content_type_name(core->content.type);
}

bool deevee_core_menu_playback_active(const struct deevee_core *core)
{
   return core && core->menu_playback_active;
}

const char *deevee_core_menu_playback_source(const struct deevee_core *core)
{
   if (!core || !core->menu_playback_source[0])
      return "none";

   return core->menu_playback_source;
}

size_t deevee_core_menu_payload_count(const struct deevee_core *core)
{
   return core ? core->menu_video_chunk_count : 0;
}

size_t deevee_core_menu_payload_bytes(const struct deevee_core *core)
{
   return core ? core->menu_video_payload_size : 0;
}

uint64_t deevee_core_menu_packets_sent(const struct deevee_core *core)
{
   return core ? core->menu_packets_sent : 0;
}

uint64_t deevee_core_menu_frames_decoded(const struct deevee_core *core)
{
   return core ? core->menu_frames_decoded : 0;
}

uint64_t deevee_core_displayed_frames(const struct deevee_core *core)
{
   return core ? core->displayed_frames : 0;
}

uint64_t deevee_core_repeated_frames(const struct deevee_core *core)
{
   return core ? core->repeated_frames : 0;
}

uint64_t deevee_core_decode_underruns(const struct deevee_core *core)
{
   return core ? core->decode_underruns : 0;
}

uint64_t deevee_core_queued_frames(const struct deevee_core *core)
{
   return core ? (uint64_t)core->frame_queue_count : 0;
}

uint64_t deevee_core_queue_drops(const struct deevee_core *core)
{
   return core ? core->queue_drops : 0;
}

uint64_t deevee_core_fallback_timing_frames(const struct deevee_core *core)
{
   return core ? core->fallback_timing_frames : 0;
}

uint64_t deevee_core_audio_payload_count(const struct deevee_core *core)
{
   return core ? (uint64_t)core->audio_chunk_count : 0;
}

uint64_t deevee_core_audio_packets_sent(const struct deevee_core *core)
{
   return core ? deevee_audio_packets_sent(&core->audio) : 0;
}

uint64_t deevee_core_audio_decoded_frames(const struct deevee_core *core)
{
   return core ? deevee_audio_decoded_frames(&core->audio) : 0;
}

uint64_t deevee_core_audio_buffered_frames(const struct deevee_core *core)
{
   return core ? (uint64_t)deevee_audio_buffered_frames(&core->audio) : 0;
}

uint64_t deevee_core_audio_decode_errors(const struct deevee_core *core)
{
   return core ? deevee_audio_decode_errors(&core->audio) : 0;
}

uint64_t deevee_core_audio_underruns(const struct deevee_core *core)
{
   return core ? deevee_audio_underruns(&core->audio) : 0;
}

unsigned deevee_core_audio_last_sample_rate(const struct deevee_core *core)
{
   return core ? deevee_audio_last_sample_rate(&core->audio) : 0;
}

unsigned deevee_core_audio_last_channels(const struct deevee_core *core)
{
   return core ? deevee_audio_last_channels(&core->audio) : 0;
}

unsigned deevee_core_video_frame_rate_code(const struct deevee_core *core)
{
   return core ? core->video_frame_rate_code : 0;
}

unsigned deevee_core_video_frame_duration_ticks(const struct deevee_core *core)
{
   return core ? core->video_frame_duration_ticks : 0;
}

unsigned deevee_core_menu_last_frame_width(const struct deevee_core *core)
{
   return core ? core->menu_last_frame_width : 0;
}

unsigned deevee_core_menu_last_frame_height(const struct deevee_core *core)
{
   return core ? core->menu_last_frame_height : 0;
}

int deevee_core_menu_last_pixel_format(const struct deevee_core *core)
{
   return core ? core->menu_last_pixel_format : 0;
}

uint8_t deevee_core_menu_button_count(const struct deevee_core *core)
{
   return core ? core->menu_button_count : 0;
}

uint8_t deevee_core_menu_active_button(const struct deevee_core *core)
{
   return core ? core->menu_active_button : 0;
}

uint8_t deevee_core_menu_confirmed_button(const struct deevee_core *core)
{
   return core ? core->menu_confirmed_button : 0;
}

uint8_t deevee_core_menu_confirmed_command_byte(
      const struct deevee_core *core, unsigned index)
{
   if (!core || !core->menu_confirmed_button ||
         core->menu_confirmed_button > core->menu_button_count ||
         index >= 8u)
      return 0;

   return core->menu_buttons[core->menu_confirmed_button - 1u].command[index];
}

bool deevee_core_menu_has_resolved_jump(const struct deevee_core *core)
{
   return core && core->menu_has_resolved_jump;
}

uint8_t deevee_core_menu_resolved_jump_command_byte(
      const struct deevee_core *core, unsigned index)
{
   if (!core || !core->menu_has_resolved_jump || index >= 8u)
      return 0;

   return core->menu_resolved_jump_command[index];
}
