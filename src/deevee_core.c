#include "deevee_core.h"
#include "deevee_dvdnav.h"

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
static void sync_dvdnav_buttons(struct deevee_core *core);
static bool append_dvdnav_events(struct deevee_core *core,
      unsigned max_events, size_t min_new_video_chunks,
      size_t min_new_audio_chunks);
static bool reset_dvdnav_decode_after_control(struct deevee_core *core);
static bool prepare_title_playback_from_target(struct deevee_core *core,
      const struct deevee_dvd_playback_target *target);
static bool prepare_menu_playback_from_vob_path(struct deevee_core *core,
      const struct deevee_content_info *content, const char *iso_path);
static bool prepare_menu_playback_from_vts_pgc(struct deevee_core *core,
      const struct deevee_content_info *content, unsigned vts,
      unsigned pgc_index);
static bool prepare_menu_playback_from_vmgm_pgc(struct deevee_core *core,
      const struct deevee_content_info *content, unsigned pgc_index);
static bool prepare_any_menu_playback(struct deevee_core *core,
      const struct deevee_content_info *content);
static void refresh_track_inventory(struct deevee_core *core);

static void update_dvdnav_position(struct deevee_core *core)
{
   if (!core || !core->dvdnav_active)
   {
      if (core)
      {
         core->dvdnav_has_position = false;
         core->dvdnav_title = 0;
         core->dvdnav_part = 0;
         core->dvdnav_parts = 0;
         core->dvdnav_time_ticks = -1;
      }
      return;
   }

   core->dvdnav_has_position = deevee_dvdnav_read_position(&core->dvdnav,
         &core->dvdnav_title, &core->dvdnav_part, &core->dvdnav_parts,
         &core->dvdnav_time_ticks);
   refresh_track_inventory(core);
}

static void language_code_to_string(uint16_t code, char out[4])
{
   if (!out)
      return;

   if (code == 0xffffu)
   {
      snprintf(out, 4, "--");
      return;
   }

   out[0] = (char)((code >> 8) & 0xffu);
   out[1] = (char)(code & 0xffu);
   out[2] = '\0';
   if (out[0] < 'a' || out[0] > 'z' || out[1] < 'a' || out[1] > 'z')
      snprintf(out, 4, "--");
}

static const char *audio_format_label(uint16_t format)
{
   switch (format)
   {
      case 0:
         return "AC3";
      case 2:
         return "MPEG-1";
      case 3:
         return "MPEG-2";
      case 4:
         return "LPCM";
      case 6:
         return "DTS";
      default:
         return "";
   }
}

static const char *audio_code_extension_label(uint8_t code_extension)
{
   switch (code_extension)
   {
      case 2:
         return "Visually impaired";
      case 3:
         return "Commentary";
      case 4:
         return "Alt commentary";
      default:
         return "";
   }
}

static const char *subpicture_code_extension_label(uint8_t code_extension)
{
   switch (code_extension)
   {
      case 2:
         return "Large";
      case 3:
         return "Children";
      case 5:
         return "Captions";
      case 6:
         return "Large captions";
      case 7:
         return "Children captions";
      case 9:
         return "Forced";
      case 13:
         return "Commentary";
      case 14:
         return "Large commentary";
      case 15:
         return "Children commentary";
      default:
         return "";
   }
}

static void append_label_part(char *label, size_t label_size,
      bool *has_detail, const char *part)
{
   size_t used;

   if (!label || !label_size || !part || !part[0])
      return;

   used = strlen(label);
   if (used >= label_size)
      return;
   snprintf(label + used, label_size - used, "%s%s",
         has_detail && *has_detail ? ", " : " (", part);
   if (has_detail)
      *has_detail = true;
}

static void finish_track_label(char *label, size_t label_size, bool has_detail)
{
   size_t used;

   if (!label || !label_size || !has_detail)
      return;

   used = strlen(label);
   if (used + 1u < label_size)
      snprintf(label + used, label_size - used, ")");
}

static void format_audio_track_label(char *label, size_t label_size,
      unsigned index, const struct deevee_dvdnav_stream_metadata *metadata)
{
   char detail[16];
   char lang[4];
   bool has_detail = false;
   const char *format;
   const char *code_extension;

   if (!label || !label_size)
      return;

   snprintf(label, label_size, "Track %u", index + 1u);
   if (!metadata)
      return;

   language_code_to_string(metadata->language, lang);
   if (strcmp(lang, "--") != 0)
      append_label_part(label, label_size, &has_detail, lang);

   format = audio_format_label(metadata->format);
   if (format[0])
      append_label_part(label, label_size, &has_detail, format);

   if (metadata->channels != 0xffffu && metadata->channels > 0u)
   {
      snprintf(detail, sizeof(detail), "%uch", (unsigned)metadata->channels);
      append_label_part(label, label_size, &has_detail, detail);
   }

   code_extension = metadata->has_code_extension ?
      audio_code_extension_label(metadata->code_extension) : "";
   if (code_extension[0])
      append_label_part(label, label_size, &has_detail, code_extension);
   finish_track_label(label, label_size, has_detail);
}

static void format_subpicture_track_label(char *label, size_t label_size,
      unsigned index, const struct deevee_dvdnav_stream_metadata *metadata)
{
   char lang[4];
   bool has_detail = false;
   const char *code_extension;

   if (!label || !label_size)
      return;

   snprintf(label, label_size, "Track %u", index + 1u);
   if (!metadata)
      return;

   language_code_to_string(metadata->language, lang);
   if (strcmp(lang, "--") != 0)
      append_label_part(label, label_size, &has_detail, lang);

   code_extension = metadata->has_code_extension ?
      subpicture_code_extension_label(metadata->code_extension) : "";
   if (code_extension[0])
      append_label_part(label, label_size, &has_detail, code_extension);
   finish_track_label(label, label_size, has_detail);
}

static void reset_track_inventory(struct deevee_core *core)
{
   if (!core)
      return;

   core->active_audio_logical_stream = -1;
   core->active_audio_physical_stream = -1;
   core->forced_audio_physical_stream = -1;
   core->audio_stream_forced = false;
   core->audio_stream_count = 0;
   memset(core->audio_streams, 0, sizeof(core->audio_streams));
   core->active_subpicture_logical_stream = -1;
   core->active_subpicture_physical_stream = -1;
   core->forced_subpicture_physical_stream = -1;
   core->subpicture_stream_forced = false;
   core->subpicture_visible = true;
   core->subpicture_stream_count = 0;
   memset(core->subpicture_streams, 0, sizeof(core->subpicture_streams));
   core->track_options_dirty = true;
}

static void refresh_track_inventory(struct deevee_core *core)
{
   unsigned i;
   int count;
   int active;

   if (!core || !core->dvdnav_active)
      return;

   count = deevee_dvdnav_stream_count(&core->dvdnav, true);
   if (count < 0)
      count = 0;
   if (count > (int)DEEVEE_MAX_AUDIO_STREAMS)
      count = (int)DEEVEE_MAX_AUDIO_STREAMS;
   if (core->audio_stream_count != (unsigned)count)
      core->track_options_dirty = true;
   core->audio_stream_count = (unsigned)count;
   for (i = 0; i < core->audio_stream_count; i++)
   {
      struct deevee_dvdnav_stream_metadata metadata;
      char label[DEEVEE_TRACK_LABEL_LENGTH];
      bool has_metadata;

      core->audio_streams[i].present = true;
      core->audio_streams[i].logical = (int)i;
      core->audio_streams[i].physical = (int)i;
      has_metadata = deevee_dvdnav_stream_metadata(&core->dvdnav, true,
            (int)i, &metadata);
      if (!has_metadata)
      {
         memset(&metadata, 0, sizeof(metadata));
         metadata.language = deevee_dvdnav_stream_language(&core->dvdnav,
               true, (int)i);
         metadata.format = 0xffffu;
         metadata.channels = 0xffffu;
      }
      core->audio_streams[i].language = metadata.language;
      core->audio_streams[i].format = metadata.format;
      core->audio_streams[i].channels = metadata.channels;
      core->audio_streams[i].code_extension = metadata.code_extension;
      core->audio_streams[i].has_code_extension =
         metadata.has_code_extension;
      format_audio_track_label(label, sizeof(label), i, &metadata);
      if (strcmp(core->audio_streams[i].label, label) != 0)
         core->track_options_dirty = true;
      snprintf(core->audio_streams[i].label,
            sizeof(core->audio_streams[i].label), "%s", label);
   }

   count = deevee_dvdnav_stream_count(&core->dvdnav, false);
   if (count < 0)
      count = 0;
   if (count > (int)DEEVEE_MAX_SUBTITLE_STREAMS)
      count = (int)DEEVEE_MAX_SUBTITLE_STREAMS;
   if (core->subpicture_stream_count != (unsigned)count)
      core->track_options_dirty = true;
   core->subpicture_stream_count = (unsigned)count;
   for (i = 0; i < core->subpicture_stream_count; i++)
   {
      struct deevee_dvdnav_stream_metadata metadata;
      char label[DEEVEE_TRACK_LABEL_LENGTH];
      bool has_metadata;

      core->subpicture_streams[i].present = true;
      core->subpicture_streams[i].logical = (int)i;
      core->subpicture_streams[i].physical = (int)i;
      has_metadata = deevee_dvdnav_stream_metadata(&core->dvdnav, false,
            (int)i, &metadata);
      if (!has_metadata)
      {
         memset(&metadata, 0, sizeof(metadata));
         metadata.language = deevee_dvdnav_stream_language(&core->dvdnav,
               false, (int)i);
         metadata.format = 0xffffu;
         metadata.channels = 0xffffu;
      }
      core->subpicture_streams[i].language = metadata.language;
      core->subpicture_streams[i].format = metadata.format;
      core->subpicture_streams[i].channels = metadata.channels;
      core->subpicture_streams[i].code_extension = metadata.code_extension;
      core->subpicture_streams[i].has_code_extension =
         metadata.has_code_extension;
      format_subpicture_track_label(label, sizeof(label), i, &metadata);
      if (strcmp(core->subpicture_streams[i].label, label) != 0)
         core->track_options_dirty = true;
      snprintf(core->subpicture_streams[i].label,
            sizeof(core->subpicture_streams[i].label), "%s", label);
   }

   active = deevee_dvdnav_active_stream(&core->dvdnav, true);
   if (active >= 0 && active < (int)DEEVEE_MAX_AUDIO_STREAMS)
   {
      if (!core->audio_stream_forced)
      {
         core->active_audio_physical_stream = active;
         core->active_audio_logical_stream = active;
         core->active_audio_stream = (uint8_t)(0x80u + (unsigned)active);
         core->has_active_audio_stream = true;
      }
   }

   active = deevee_dvdnav_active_stream(&core->dvdnav, false);
   if (active >= 0 && active < (int)DEEVEE_MAX_SUBTITLE_STREAMS)
   {
      if (!core->subpicture_stream_forced)
      {
         core->active_subpicture_physical_stream = active;
         core->active_subpicture_logical_stream = active;
         core->active_subpicture_stream = (uint8_t)(0x20u + (unsigned)active);
         core->has_active_subpicture_stream = true;
      }
   }
}

static bool content_is_disc_image(const struct deevee_content_info *content)
{
   return content && (content->type == DEEVEE_CONTENT_ISO ||
         content->type == DEEVEE_CONTENT_CHD);
}

static void absorb_current_nav_input(struct deevee_core *core)
{
   if (!core)
      return;

   core->menu_last_nav_mask = deevee_nav_active_mask(&core->nav);
}

static uint32_t deevee_core_rgb(unsigned r, unsigned g, unsigned b)
{
   return 0xff000000u | ((r & 0xffu) << 16) |
      ((g & 0xffu) << 8) | (b & 0xffu);
}

static uint32_t deevee_core_argb(unsigned a, unsigned r, unsigned g,
      unsigned b)
{
   return ((a & 0xffu) << 24) | ((r & 0xffu) << 16) |
      ((g & 0xffu) << 8) | (b & 0xffu);
}

static uint16_t read_be16(const uint8_t *data)
{
   return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
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

static unsigned frame_duration_with_repeat_pict(const struct deevee_core *core,
      int repeat_pict)
{
   unsigned base = fallback_frame_duration_ticks(core);

   if (repeat_pict <= 0)
      return base;
   return clamp_frame_duration_ticks(base +
      ((uint64_t)base * (uint64_t)repeat_pict + 1u) / 2u);
}

static unsigned display_hold_ticks_from_duration(struct deevee_core *core,
      unsigned duration)
{
   unsigned display_runs;
   uint64_t accumulated;

   if (!duration)
      return 0;

   accumulated = (uint64_t)duration +
      (core ? core->display_frame_tick_remainder : 0u);
   display_runs = (unsigned)(accumulated / DEEVEE_CLOCK_TICKS_PER_RUN);
   if (display_runs < 1u)
      display_runs = 1u;
   if (core)
      core->display_frame_tick_remainder =
         (unsigned)(accumulated % DEEVEE_CLOCK_TICKS_PER_RUN);

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
      core->frame_queue[i].repeat_pict = 0;
      core->frame_queue[i].width = 0;
      core->frame_queue[i].height = 0;
      core->frame_queue[i].pixel_format = 0;
   }

   core->frame_queue_head = 0;
   core->frame_queue_count = 0;
   core->frame_clock_pts = 0;
   core->frame_clock_has_pts = false;
   core->display_frame_ticks_remaining = 0;
   core->display_frame_tick_remainder = 0;
   core->displayed_frames = 0;
   core->repeated_frames = 0;
   core->decode_underruns = 0;
   core->queue_drops = 0;
   core->fallback_timing_frames = 0;
   core->last_decoded_frame_duration_ticks = 0;
   core->last_decoded_repeat_pict = 0;
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
   if (!core->menu_base_pixels)
   {
      core->menu_base_pixels = (uint32_t *)calloc(
            DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT, sizeof(uint32_t));
      if (!core->menu_base_pixels)
         return false;
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
   free(core->menu_base_pixels);
   core->menu_base_pixels = NULL;
   core->menu_base_pixels_valid = false;
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
   free(core->subpicture_payloads);
   free(core->subpicture_chunks);
   free(core->subpicture_assembly);
   deevee_subpicture_frame_clear(&core->subpicture_frame);
   deevee_subpicture_deinit(&core->subpicture);
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
   core->active_audio_stream = 0;
   core->has_active_audio_stream = false;
   core->subpicture_payloads = NULL;
   core->subpicture_payload_size = 0;
   core->subpicture_payload_capacity = 0;
   core->subpicture_chunks = NULL;
   core->subpicture_chunk_count = 0;
   core->subpicture_chunk_capacity = 0;
   core->next_subpicture_chunk = 0;
   core->subpicture_payload_packets = 0;
   core->subpicture_assembly = NULL;
   core->subpicture_assembly_size = 0;
   core->subpicture_assembly_capacity = 0;
   core->subpicture_assembly_expected_size = 0;
   core->subpicture_assembly_has_pts = false;
   core->subpicture_assembly_pts = 0;
   deevee_subpicture_init(&core->subpicture);
   core->active_subpicture_stream = 0;
   core->has_active_subpicture_stream = false;
   core->subpicture_frame_ready = false;
   core->subpicture_decode_errors = 0;
   reset_track_inventory(core);
   core->active_button_color_table = 0;
   core->has_active_button_color_table = false;
   memset(core->subpicture_clut, 0, sizeof(core->subpicture_clut));
   memset(core->select_color_table, 0, sizeof(core->select_color_table));
   core->has_subpicture_clut = false;
   core->has_select_color_table = false;
   core->menu_base_pixels_valid = false;
   deevee_audio_deinit(&core->audio);
   deevee_audio_init(&core->audio);
   core->menu_frame_hold = 0;
   core->menu_frame_repeat = DEEVEE_DEFAULT_MENU_FRAME_REPEAT;
   core->video_aspect_ratio_code = 0;
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
   core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_NONE;
   core->menu_command_status[0] = '\0';
   core->menu_at_end = false;
   core->menu_loop_enabled = false;
   core->playback_is_title = false;
   core->playback_paused = false;
   core->menu_last_frame_has_pts = false;
   core->menu_last_frame_pts = 0;
   core->menu_previous_frame_has_pts = false;
   core->menu_previous_frame_pts = 0;
   deevee_dvdnav_close(&core->dvdnav);
   core->dvdnav_active = false;
   core->dvdnav_has_position = false;
   core->dvdnav_title = 0;
   core->dvdnav_part = 0;
   core->dvdnav_parts = 0;
   core->dvdnav_time_ticks = -1;
   reset_frame_queue(core);
}

static void clear_stream_buffers_keep_dvdnav(struct deevee_core *core)
{
   bool audio_stream_forced;
   int forced_audio_physical_stream;
   bool subpicture_stream_forced;
   int forced_subpicture_physical_stream;
   bool subpicture_visible;

   if (!core)
      return;

   audio_stream_forced = core->audio_stream_forced;
   forced_audio_physical_stream = core->forced_audio_physical_stream;
   subpicture_stream_forced = core->subpicture_stream_forced;
   forced_subpicture_physical_stream = core->forced_subpicture_physical_stream;
   subpicture_visible = core->subpicture_visible;

   deevee_decoder_deinit(&core->decoder);
   free(core->menu_video_payloads);
   free(core->menu_video_chunks);
   free(core->audio_payloads);
   free(core->audio_chunks);
   free(core->subpicture_payloads);
   free(core->subpicture_chunks);
   free(core->subpicture_assembly);
   deevee_subpicture_frame_clear(&core->subpicture_frame);
   deevee_subpicture_deinit(&core->subpicture);
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
   core->active_audio_stream = 0;
   core->has_active_audio_stream = false;
   core->subpicture_payloads = NULL;
   core->subpicture_payload_size = 0;
   core->subpicture_payload_capacity = 0;
   core->subpicture_chunks = NULL;
   core->subpicture_chunk_count = 0;
   core->subpicture_chunk_capacity = 0;
   core->next_subpicture_chunk = 0;
   core->subpicture_payload_packets = 0;
   core->subpicture_assembly = NULL;
   core->subpicture_assembly_size = 0;
   core->subpicture_assembly_capacity = 0;
   core->subpicture_assembly_expected_size = 0;
   core->subpicture_assembly_has_pts = false;
   core->subpicture_assembly_pts = 0;
   deevee_subpicture_init(&core->subpicture);
   core->active_subpicture_stream = 0;
   core->has_active_subpicture_stream = false;
   core->subpicture_frame_ready = false;
   core->subpicture_decode_errors = 0;
   reset_track_inventory(core);
   core->subpicture_visible = subpicture_visible;
   if (audio_stream_forced && forced_audio_physical_stream >= 0 &&
         forced_audio_physical_stream < (int)DEEVEE_MAX_AUDIO_STREAMS)
   {
      core->audio_stream_forced = true;
      core->forced_audio_physical_stream = forced_audio_physical_stream;
      core->active_audio_physical_stream = forced_audio_physical_stream;
      core->active_audio_logical_stream = forced_audio_physical_stream;
      core->active_audio_stream =
         (uint8_t)(0x80u + (unsigned)forced_audio_physical_stream);
      core->has_active_audio_stream = true;
   }
   if (subpicture_stream_forced && forced_subpicture_physical_stream >= 0 &&
         forced_subpicture_physical_stream < (int)DEEVEE_MAX_SUBTITLE_STREAMS)
   {
      core->subpicture_stream_forced = true;
      core->forced_subpicture_physical_stream =
         forced_subpicture_physical_stream;
      core->active_subpicture_physical_stream =
         forced_subpicture_physical_stream;
      core->active_subpicture_logical_stream =
         forced_subpicture_physical_stream;
      core->active_subpicture_stream =
         (uint8_t)(0x20u + (unsigned)forced_subpicture_physical_stream);
      core->has_active_subpicture_stream = true;
   }
   core->active_button_color_table = 0;
   core->has_active_button_color_table = false;
   memset(core->subpicture_clut, 0, sizeof(core->subpicture_clut));
   memset(core->select_color_table, 0, sizeof(core->select_color_table));
   core->has_subpicture_clut = false;
   core->has_select_color_table = false;
   core->menu_base_pixels_valid = false;
   deevee_audio_deinit(&core->audio);
   deevee_audio_init(&core->audio);
   core->menu_frame_hold = 0;
   core->menu_packets_sent = 0;
   core->menu_frames_decoded = 0;
   core->video_aspect_ratio_code = 0;
   core->menu_button_count = 0;
   core->menu_active_button = 0;
   core->menu_confirmed_button = 0;
   memset(core->menu_buttons, 0, sizeof(core->menu_buttons));
   core->menu_at_end = false;
   core->menu_loop_enabled = false;
   core->playback_paused = false;
   core->menu_last_frame_has_pts = false;
   core->menu_last_frame_pts = 0;
   core->menu_previous_frame_has_pts = false;
   core->menu_previous_frame_pts = 0;
   core->display_frame_ticks_remaining = 0;
   core->display_frame_tick_remainder = 0;
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

   if (core->has_active_audio_stream &&
         payload[0] != core->active_audio_stream)
      return true;

   if (!core->has_active_audio_stream)
   {
      core->active_audio_stream = payload[0];
      core->has_active_audio_stream = true;
   }

   return append_audio_payload(core, payload + 4u, packet->payload_size - 4u);
}

static void reset_audio_stream_buffers(struct deevee_core *core)
{
   if (!core)
      return;

   core->audio_payload_size = 0;
   core->audio_chunk_count = 0;
   core->next_audio_chunk = 0;
   core->audio_payload_packets = 0;
   deevee_audio_deinit(&core->audio);
   deevee_audio_init(&core->audio);
}

static void reset_subpicture_stream_buffers(struct deevee_core *core)
{
   if (!core)
      return;

   core->subpicture_payload_size = 0;
   core->subpicture_chunk_count = 0;
   core->next_subpicture_chunk = 0;
   core->subpicture_payload_packets = 0;
   core->subpicture_assembly_size = 0;
   core->subpicture_assembly_expected_size = 0;
   core->subpicture_assembly_has_pts = false;
   core->subpicture_assembly_pts = 0;
   core->subpicture_frame_ready = false;
   deevee_subpicture_frame_clear(&core->subpicture_frame);
   deevee_subpicture_deinit(&core->subpicture);
   deevee_subpicture_init(&core->subpicture);
}

static void apply_audio_stream_change(struct deevee_core *core, int physical,
      int logical)
{
   int packet_stream;
   uint8_t stream_id;
   bool changed;

   if (!core || core->audio_stream_forced)
      return;

   packet_stream = logical >= 0 && logical < (int)DEEVEE_MAX_AUDIO_STREAMS ?
      logical : physical;
   if (packet_stream < 0 || packet_stream >= (int)DEEVEE_MAX_AUDIO_STREAMS)
      return;

   stream_id = (uint8_t)(0x80u + (unsigned)packet_stream);
   changed = core->has_active_audio_stream &&
      core->active_audio_stream != stream_id;
   core->active_audio_physical_stream = packet_stream;
   core->active_audio_logical_stream = logical;
   core->active_audio_stream = stream_id;
   core->has_active_audio_stream = true;
   if (changed)
      reset_audio_stream_buffers(core);
   core->track_options_dirty = true;
}

static void apply_subpicture_stream_change(struct deevee_core *core,
      int physical, int logical)
{
   uint8_t stream_id;
   bool changed;

   if (!core || physical < 0 ||
         physical >= (int)DEEVEE_MAX_SUBTITLE_STREAMS ||
         core->subpicture_stream_forced)
      return;

   stream_id = (uint8_t)(0x20u + (unsigned)physical);
   changed = core->has_active_subpicture_stream &&
      core->active_subpicture_stream != stream_id;
   core->active_subpicture_physical_stream = physical;
   core->active_subpicture_logical_stream = logical;
   core->active_subpicture_stream = stream_id;
   core->has_active_subpicture_stream = true;
   core->subpicture_visible = true;
   if (changed)
      reset_subpicture_stream_buffers(core);
   core->track_options_dirty = true;
}

static bool append_subpicture_payload(struct deevee_core *core,
      const struct deevee_dvd_packet *packet, uint8_t stream_id,
      const uint8_t *payload, size_t payload_size)
{
   uint8_t *new_payloads;
   struct deevee_subpicture_payload_chunk *new_chunks;
   size_t new_payload_capacity;
   size_t new_chunk_capacity;

   if (!core || !packet || !payload || !payload_size)
      return false;

   if (core->subpicture_payload_size + payload_size >
         core->subpicture_payload_capacity)
   {
      new_payload_capacity = core->subpicture_payload_capacity ?
         core->subpicture_payload_capacity * 2u : 4096u;
      while (new_payload_capacity < core->subpicture_payload_size +
            payload_size)
         new_payload_capacity *= 2u;

      new_payloads = (uint8_t *)realloc(core->subpicture_payloads,
            new_payload_capacity);
      if (!new_payloads)
         return false;

      core->subpicture_payloads = new_payloads;
      core->subpicture_payload_capacity = new_payload_capacity;
   }

   if (core->subpicture_chunk_count + 1u > core->subpicture_chunk_capacity)
   {
      new_chunk_capacity = core->subpicture_chunk_capacity ?
         core->subpicture_chunk_capacity * 2u : 16u;
      new_chunks = (struct deevee_subpicture_payload_chunk *)realloc(
            core->subpicture_chunks, new_chunk_capacity *
            sizeof(*core->subpicture_chunks));
      if (!new_chunks)
         return false;

      core->subpicture_chunks = new_chunks;
      core->subpicture_chunk_capacity = new_chunk_capacity;
   }

   memcpy(core->subpicture_payloads + core->subpicture_payload_size, payload,
         payload_size);
   core->subpicture_chunks[core->subpicture_chunk_count].offset =
      core->subpicture_payload_size;
   core->subpicture_chunks[core->subpicture_chunk_count].size = payload_size;
   core->subpicture_chunks[core->subpicture_chunk_count].has_pts =
      packet->has_pts;
   core->subpicture_chunks[core->subpicture_chunk_count].pts =
      packet->has_pts ? (int64_t)packet->pts : 0;
   core->subpicture_chunks[core->subpicture_chunk_count].stream_id =
      stream_id;
   core->subpicture_payload_size += payload_size;
   core->subpicture_chunk_count++;
   core->subpicture_payload_packets++;
   return true;
}

static bool append_subpicture_packet(struct deevee_core *core,
      const struct deevee_dvd_packet *packet)
{
   const uint8_t *payload;

   if (!core || !packet || packet->stream_id != 0xbd ||
         !packet->payload || packet->payload_size <= 1u)
      return true;

   payload = packet->payload;
   if (payload[0] < 0x20 || payload[0] > 0x3f)
      return true;

   if (!core->subpicture_visible && core->playback_is_title)
      return true;

   if (core->has_active_subpicture_stream &&
         payload[0] != core->active_subpicture_stream)
      return true;

   if (!core->has_active_subpicture_stream)
   {
      core->active_subpicture_stream = payload[0];
      core->has_active_subpicture_stream = true;
   }

   return append_subpicture_payload(core, packet, payload[0], payload + 1u,
         packet->payload_size - 1u);
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
   core->video_aspect_ratio_code = 0;
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
            core->video_aspect_ratio_code = payload[i + 7] >> 4;
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
   core->menu_at_end = false;
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
   core->display_frame_tick_remainder = 0;
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
   if (probe->has_subpicture_clut)
   {
      memcpy(core->subpicture_clut, probe->subpicture_clut,
            sizeof(core->subpicture_clut));
      core->has_subpicture_clut = true;
   }
   if (probe->has_select_color_table)
   {
      memcpy(core->select_color_table, probe->select_color_table,
            sizeof(core->select_color_table));
      core->has_select_color_table = true;
   }
   if (probe->first_subpicture_stream_id)
   {
      core->active_subpicture_stream = probe->first_subpicture_stream_id;
      core->has_active_subpicture_stream = true;
   }

   initial_button = probe->forced_select_button;
   if (!initial_button)
      initial_button = probe->starting_button;
   if (!initial_button || initial_button > probe->button_count)
      initial_button = 1;

   core->menu_active_button = initial_button;
   core->active_button_color_table =
      core->menu_buttons[initial_button - 1u].color_table;
   core->has_active_button_color_table = true;
}

static void format_command_status(struct deevee_core *core,
      const char *prefix, const uint8_t command[8])
{
   if (!core)
      return;

   if (!prefix)
      prefix = "command";

   if (!command)
   {
      snprintf(core->menu_command_status, sizeof(core->menu_command_status),
            "%s:none", prefix);
      return;
   }

   snprintf(core->menu_command_status, sizeof(core->menu_command_status),
         "%s:%02x%02x%02x%02x%02x%02x%02x%02x", prefix,
         command[0], command[1], command[2], command[3],
         command[4], command[5], command[6], command[7]);
}

static void format_dvdnav_control_status(struct deevee_core *core,
      const char *control, bool searched, int before_title, int before_part,
      int after_title, int after_part)
{
   if (!core)
      return;

   snprintf(core->menu_command_status, sizeof(core->menu_command_status),
         "dvdnav:%s:%s:t%d/p%d->t%d/p%d",
         control ? control : "control", searched ? "ok" : "failed",
         before_title, before_part, after_title, after_part);
}

static bool is_link_tail_pgc_command(const uint8_t command[8])
{
   return command && command[0] == 0x20 && (command[1] & 0x0fu) == 0x01 &&
      (command[7] & 0x1fu) == 0x0d;
}

static bool decode_menu_button_command(struct deevee_core *core,
      const uint8_t command[8],
      const struct deevee_dvd_title_table *title_table,
      struct deevee_dvd_playback_target *target)
{
   uint8_t command_index;

   if (!core || !command || !target)
      return false;

   memset(core->menu_resolved_jump_command, 0,
         sizeof(core->menu_resolved_jump_command));
   core->menu_has_resolved_jump = false;

   if (deevee_dvd_decode_playback_target_command(command,
            core->menu_current_vts, core->menu_domain, title_table, target))
   {
      memcpy(core->menu_resolved_jump_command, command,
            sizeof(core->menu_resolved_jump_command));
      core->menu_has_resolved_jump = true;
      return true;
   }

   if (!is_link_tail_pgc_command(command))
      return false;

   for (command_index = 0; command_index < core->menu_post_command_count;
         command_index++)
   {
      if (deevee_dvd_decode_playback_target_command(
               core->menu_post_commands[command_index],
               core->menu_current_vts, core->menu_domain, title_table, target))
      {
         memcpy(core->menu_resolved_jump_command,
               core->menu_post_commands[command_index],
               sizeof(core->menu_resolved_jump_command));
         core->menu_has_resolved_jump = true;
         return true;
      }
   }

   return false;
}

static bool prepare_vts_menu_playback_from_target(struct deevee_core *core,
      const struct deevee_dvd_playback_target *target)
{
   unsigned pgc_index;
   char path[32];

   if (!core || !target ||
         target->type != DEEVEE_DVD_PLAYBACK_TARGET_MENU ||
         target->menu_domain != DEEVEE_DVD_MENU_DOMAIN_VTS ||
         !target->vts_number)
      return false;

   if (target->pgc_number)
      return prepare_menu_playback_from_vts_pgc(core, &core->content,
            target->vts_number, target->pgc_number - 1u);

   for (pgc_index = 0; pgc_index < 32u; pgc_index++)
      if (prepare_menu_playback_from_vts_pgc(core, &core->content,
               target->vts_number, pgc_index))
         return true;

   snprintf(path, sizeof(path), "/VIDEO_TS/VTS_%02u_0.VOB",
         (unsigned)target->vts_number);
   if (prepare_menu_playback_from_vob_path(core, &core->content, path))
   {
      core->menu_current_vts = target->vts_number;
      core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_VTS;
      return true;
   }

   return false;
}

static bool resolve_buttonless_menu_post_target(struct deevee_disc *disc,
      const struct deevee_dvd_title_table *title_table,
      struct deevee_dvd_playback_target *target)
{
   unsigned depth;

   if (!disc || !title_table || !target)
      return false;

   for (depth = 0; depth < 4u; depth++)
   {
      struct deevee_dvd_menu_render_probe probe;
      enum deevee_dvd_status status;
      uint8_t command_index;
      bool followed = false;

      if (target->type != DEEVEE_DVD_PLAYBACK_TARGET_MENU ||
            !target->pgc_number)
         return depth > 0;

      memset(&probe, 0, sizeof(probe));
      if (target->menu_domain == DEEVEE_DVD_MENU_DOMAIN_VMG)
         status = deevee_dvd_probe_vmgm_menu_pgc_render_streams(disc,
               target->pgc_number - 1u, &probe);
      else if (target->menu_domain == DEEVEE_DVD_MENU_DOMAIN_VTS &&
            target->vts_number)
         status = deevee_dvd_probe_vts_menu_pgc_render_streams(disc,
               target->vts_number, target->pgc_number - 1u, &probe);
      else
         return depth > 0;

      if (status != DEEVEE_DVD_OK || probe.button_count ||
            !probe.parsed_post_command_count)
         return depth > 0;

      for (command_index = 0; command_index < probe.parsed_post_command_count;
            command_index++)
      {
         struct deevee_dvd_playback_target follow_target;
         unsigned current_vts = target->menu_domain ==
            DEEVEE_DVD_MENU_DOMAIN_VTS ? target->vts_number : 0;

         memset(&follow_target, 0, sizeof(follow_target));
         if (!deevee_dvd_decode_playback_target_command(
                  probe.post_commands[command_index], current_vts,
                  target->menu_domain, title_table, &follow_target))
            continue;

         *target = follow_target;
         followed = true;
         break;
      }

      if (!followed)
         return depth > 0;
   }

   return true;
}

static bool dispatch_menu_button_command(struct deevee_core *core,
      const uint8_t command[8])
{
   struct deevee_disc disc;
   struct deevee_dvd_info dvd_info;
   struct deevee_dvd_title_table title_table;
   struct deevee_dvd_playback_target target;
   bool dispatched = false;

   if (!core || !command || !content_is_disc_image(&core->content))
      return false;

   format_command_status(core, "unsupported", command);
   memset(&dvd_info, 0, sizeof(dvd_info));
   memset(&title_table, 0, sizeof(title_table));
   memset(&target, 0, sizeof(target));

   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, &core->content) != DEEVEE_DISC_OK)
      return false;

   if (deevee_dvd_probe(&disc, &dvd_info) != DEEVEE_DVD_OK ||
         deevee_dvd_read_title_table(&disc, &dvd_info, &title_table) !=
            DEEVEE_DVD_OK ||
         !decode_menu_button_command(core, command, &title_table, &target))
      goto end_disc;

   (void)resolve_buttonless_menu_post_target(&disc, &title_table, &target);

   switch (target.type)
   {
      case DEEVEE_DVD_PLAYBACK_TARGET_TITLE:
         dispatched = prepare_title_playback_from_target(core, &target);
         break;
      case DEEVEE_DVD_PLAYBACK_TARGET_MENU:
         if (target.menu_domain == DEEVEE_DVD_MENU_DOMAIN_VTS)
            dispatched = prepare_vts_menu_playback_from_target(core, &target);
         else if (target.menu_domain == DEEVEE_DVD_MENU_DOMAIN_VMG &&
               target.pgc_number)
         {
            dispatched = prepare_menu_playback_from_vmgm_pgc(core,
                  &core->content, target.pgc_number - 1u);
         }
         if (dispatched)
            core->menu_playback_active = true;
         break;
      default:
         break;
   }

   if (dispatched && target.type == DEEVEE_DVD_PLAYBACK_TARGET_MENU &&
         !core->menu_button_count && core->menu_post_command_count)
   {
      uint8_t command_index;

      for (command_index = 0; command_index < core->menu_post_command_count;
            command_index++)
      {
         struct deevee_dvd_playback_target follow_target;
         bool follow_prepared = false;

         memset(&follow_target, 0, sizeof(follow_target));
         if (!deevee_dvd_decode_playback_target_command(
                  core->menu_post_commands[command_index],
                  core->menu_current_vts, core->menu_domain,
                  &title_table, &follow_target))
            continue;

         if (follow_target.type == DEEVEE_DVD_PLAYBACK_TARGET_TITLE)
            follow_prepared = prepare_title_playback_from_target(core,
                  &follow_target);
         else if (follow_target.type == DEEVEE_DVD_PLAYBACK_TARGET_MENU &&
               follow_target.menu_domain == DEEVEE_DVD_MENU_DOMAIN_VTS)
            follow_prepared = prepare_vts_menu_playback_from_target(core,
                  &follow_target);
         else if (follow_target.type == DEEVEE_DVD_PLAYBACK_TARGET_MENU &&
               follow_target.menu_domain == DEEVEE_DVD_MENU_DOMAIN_VMG &&
               follow_target.pgc_number)
            follow_prepared = prepare_menu_playback_from_vmgm_pgc(core,
                  &core->content, follow_target.pgc_number - 1u);

         if (follow_prepared)
         {
            target = follow_target;
            if (target.type == DEEVEE_DVD_PLAYBACK_TARGET_MENU)
               core->menu_playback_active = true;
            break;
         }
      }
   }

   if (dispatched)
   {
      core->menu_confirmed_button = 0;
      snprintf(core->menu_command_status, sizeof(core->menu_command_status),
            "%s:%s:%u", deevee_dvd_playback_target_type_name(target.type),
            deevee_dvd_menu_domain_name(target.menu_domain),
            target.type == DEEVEE_DVD_PLAYBACK_TARGET_MENU ?
               target.pgc_number : target.vts_title_number);
   }

end_disc:
   deevee_disc_close(&disc);
   return dispatched;
}

static void update_menu_button_selection(struct deevee_core *core)
{
   uint32_t nav_mask;
   uint32_t pressed;
   const struct deevee_dvd_menu_button *button;
   uint8_t next_button = 0;
   bool dvdnav_selection_changed = false;

   if (!core)
      return;

   update_dvdnav_position(core);
   if (core->dvdnav_active)
      sync_dvdnav_buttons(core);

   nav_mask = deevee_nav_active_mask(&core->nav);
   pressed = nav_mask & ~core->menu_last_nav_mask;
   core->menu_last_nav_mask = nav_mask;

   if (pressed & ((uint32_t)1u << DEEVEE_NAV_HOME))
   {
      bool prepared = false;

      if (core->dvdnav_active)
      {
         bool called = deevee_dvdnav_menu_call_root(&core->dvdnav);

         snprintf(core->menu_command_status,
               sizeof(core->menu_command_status), "dvdnav:home:%s",
               called ? "ok" : "failed");
         if (called)
            prepared = reset_dvdnav_decode_after_control(core);
      }

      if (!prepared && content_is_disc_image(&core->content))
      {
         prepared = prepare_any_menu_playback(core, &core->content);
         core->menu_playback_active = prepared;
         snprintf(core->menu_command_status,
               sizeof(core->menu_command_status), "home:fallback:%s",
               prepared ? "ok" : "failed");
      }
      if (prepared)
         absorb_current_nav_input(core);
      return;
   }
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_PLAY_PAUSE))
   {
      core->playback_paused = !core->playback_paused;
      if (core->playback_paused)
         deevee_audio_reset(&core->audio);
      snprintf(core->menu_command_status, sizeof(core->menu_command_status),
            "transport:%s", core->playback_paused ? "paused" : "playing");
      return;
   }
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_STOP))
   {
      core->playback_paused = !core->playback_paused;
      if (core->playback_paused)
         deevee_audio_reset(&core->audio);
      snprintf(core->menu_command_status, sizeof(core->menu_command_status),
            "transport:%s", core->playback_paused ? "paused" : "playing");
      return;
   }
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_PREVIOUS_CHAPTER))
   {
      if (core->dvdnav_active)
      {
         int before_title = core->dvdnav_title;
         int before_part = core->dvdnav_part;
         int after_title;
         int after_part;
         bool searched = deevee_dvdnav_previous_chapter(&core->dvdnav);

         if (searched)
         {
            core->playback_paused = false;
            (void)reset_dvdnav_decode_after_control(core);
         }
         update_dvdnav_position(core);
         after_title = core->dvdnav_title;
         after_part = core->dvdnav_part;
         format_dvdnav_control_status(core, "prev", searched,
               before_title, before_part, after_title, after_part);
      }
      return;
   }
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_NEXT_CHAPTER))
   {
      if (core->dvdnav_active)
      {
         int before_title = core->dvdnav_title;
         int before_part = core->dvdnav_part;
         int after_title;
         int after_part;
         bool searched = deevee_dvdnav_next_chapter(&core->dvdnav);

         if (searched)
         {
            core->playback_paused = false;
            (void)reset_dvdnav_decode_after_control(core);
         }
         update_dvdnav_position(core);
         after_title = core->dvdnav_title;
         after_part = core->dvdnav_part;
         format_dvdnav_control_status(core, "next", searched,
               before_title, before_part, after_title, after_part);
      }
      return;
   }
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_REWIND))
   {
      if (core->dvdnav_active)
      {
         int before_title = core->dvdnav_title;
         int before_part = core->dvdnav_part;
         int after_title;
         int after_part;
         bool searched = deevee_dvdnav_scan_seconds(&core->dvdnav, -10);

         if (searched)
         {
            core->playback_paused = false;
            (void)reset_dvdnav_decode_after_control(core);
         }
         update_dvdnav_position(core);
         after_title = core->dvdnav_title;
         after_part = core->dvdnav_part;
         format_dvdnav_control_status(core, "rewind", searched,
               before_title, before_part, after_title, after_part);
      }
      return;
   }
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_FAST_FORWARD))
   {
      if (core->dvdnav_active)
      {
         int before_title = core->dvdnav_title;
         int before_part = core->dvdnav_part;
         int after_title;
         int after_part;
         bool searched = deevee_dvdnav_scan_seconds(&core->dvdnav, 10);

         if (searched)
         {
            core->playback_paused = false;
            (void)reset_dvdnav_decode_after_control(core);
         }
         update_dvdnav_position(core);
         after_title = core->dvdnav_title;
         after_part = core->dvdnav_part;
         format_dvdnav_control_status(core, "fast_forward", searched,
               before_title, before_part, after_title, after_part);
      }
      return;
   }

   if (!core->menu_button_count || !core->menu_active_button ||
         core->menu_active_button > core->menu_button_count)
      return;

   button = &core->menu_buttons[core->menu_active_button - 1u];
   if (pressed & ((uint32_t)1u << DEEVEE_NAV_UP))
   {
      if (core->dvdnav_active)
         dvdnav_selection_changed = deevee_dvdnav_button(&core->dvdnav,
               DEEVEE_DVDNAV_BUTTON_UP);
      next_button = button->up;
   }
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_DOWN))
   {
      if (core->dvdnav_active)
         dvdnav_selection_changed = deevee_dvdnav_button(&core->dvdnav,
               DEEVEE_DVDNAV_BUTTON_DOWN);
      next_button = button->down;
   }
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_LEFT))
   {
      if (core->dvdnav_active)
         dvdnav_selection_changed = deevee_dvdnav_button(&core->dvdnav,
               DEEVEE_DVDNAV_BUTTON_LEFT);
      next_button = button->left;
   }
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_RIGHT))
   {
      if (core->dvdnav_active)
         dvdnav_selection_changed = deevee_dvdnav_button(&core->dvdnav,
               DEEVEE_DVDNAV_BUTTON_RIGHT);
      next_button = button->right;
   }
   else if (pressed & ((uint32_t)1u << DEEVEE_NAV_CONFIRM))
   {
      bool fallback_prepared = false;

      if (core->dvdnav_active)
      {
         int before_title = core->dvdnav_title;
         int before_part = core->dvdnav_part;
         bool activated = deevee_dvdnav_activate_button(&core->dvdnav,
               core->menu_active_button);
         bool prepared = false;

         if (activated)
            prepared = reset_dvdnav_decode_after_control(core);

         if (prepared)
         {
            snprintf(core->menu_command_status,
                  sizeof(core->menu_command_status),
                  "dvdnav:activated:t%d/p%d->t%d/p%d",
                  before_title, before_part, core->dvdnav_title,
                  core->dvdnav_part);
            absorb_current_nav_input(core);
            return;
         }

         snprintf(core->menu_command_status,
               sizeof(core->menu_command_status), "dvdnav:%s:fallback",
               activated ? "empty" : "activate_failed");
      }
      core->menu_confirmed_button = core->menu_active_button;
      fallback_prepared = dispatch_menu_button_command(core, button->command);
      if (fallback_prepared)
         absorb_current_nav_input(core);
      if (!fallback_prepared && !core->dvdnav_active)
         format_command_status(core, "dispatch_failed", button->command);
   }

   if (next_button && next_button <= core->menu_button_count)
   {
      core->menu_active_button = next_button;
      core->active_button_color_table =
         core->menu_buttons[core->menu_active_button - 1u].color_table;
      core->has_active_button_color_table = true;
      if (core->dvdnav_active && !dvdnav_selection_changed)
         dvdnav_selection_changed = deevee_dvdnav_select_button(&core->dvdnav,
               core->menu_active_button);
   }
   if (core->dvdnav_active && dvdnav_selection_changed)
      sync_dvdnav_buttons(core);
}

static void draw_debug_digit(struct deevee_core *core, unsigned digit,
      unsigned x0, unsigned y0, uint32_t color)
{
   static const uint8_t font[10][5] = {
      { 0x7, 0x5, 0x5, 0x5, 0x7 },
      { 0x2, 0x6, 0x2, 0x2, 0x7 },
      { 0x7, 0x1, 0x7, 0x4, 0x7 },
      { 0x7, 0x1, 0x7, 0x1, 0x7 },
      { 0x5, 0x5, 0x7, 0x1, 0x1 },
      { 0x7, 0x4, 0x7, 0x1, 0x7 },
      { 0x7, 0x4, 0x7, 0x5, 0x7 },
      { 0x7, 0x1, 0x1, 0x1, 0x1 },
      { 0x7, 0x5, 0x7, 0x5, 0x7 },
      { 0x7, 0x5, 0x7, 0x1, 0x7 }
   };
   unsigned row;
   unsigned col;
   unsigned scale_y;
   unsigned scale_x;

   if (!core || !core->video.pixels || digit > 9u)
      return;

   for (row = 0; row < 5u; row++)
      for (col = 0; col < 3u; col++)
         if (font[digit][row] & (1u << (2u - col)))
            for (scale_y = 0; scale_y < 2u; scale_y++)
               for (scale_x = 0; scale_x < 2u; scale_x++)
               {
                  unsigned x = x0 + col * 2u + scale_x;
                  unsigned y = y0 + row * 2u + scale_y;

                  if (x < DEEVEE_VIDEO_WIDTH && y < DEEVEE_VIDEO_HEIGHT)
                     core->video.pixels[y * DEEVEE_VIDEO_WIDTH + x] = color;
               }
}

static void draw_debug_button_number(struct deevee_core *core, unsigned number,
      unsigned x, unsigned y, uint32_t color)
{
   if (!core || !number)
      return;

   if (number >= 10u)
   {
      draw_debug_digit(core, number / 10u, x, y, color);
      x += 8u;
   }
   draw_debug_digit(core, number % 10u, x, y, color);
}

static void draw_menu_button_overlay(struct deevee_core *core)
{
   unsigned button_index;
   unsigned x;
   unsigned y;

   if (!core || !core->video.pixels || !core->menu_button_count)
      return;

   for (button_index = 0; button_index < core->menu_button_count;
         button_index++)
   {
      const struct deevee_dvd_menu_button *button =
         &core->menu_buttons[button_index];
      uint32_t color;
      uint32_t label_color;
      uint16_t x0;
      uint16_t x1;
      uint16_t y0;
      uint16_t y1;
      unsigned thickness;
      unsigned bracket;
      unsigned label_x;
      unsigned label_y;
      unsigned button_number = button_index + 1u;
      bool active = button_number == core->menu_active_button;

      x0 = button->x_start < DEEVEE_VIDEO_WIDTH ? button->x_start :
         DEEVEE_VIDEO_WIDTH - 1u;
      x1 = button->x_end < DEEVEE_VIDEO_WIDTH ? button->x_end :
         DEEVEE_VIDEO_WIDTH - 1u;
      y0 = button->y_start < DEEVEE_VIDEO_HEIGHT ? button->y_start :
         DEEVEE_VIDEO_HEIGHT - 1u;
      y1 = button->y_end < DEEVEE_VIDEO_HEIGHT ? button->y_end :
         DEEVEE_VIDEO_HEIGHT - 1u;

      if (x1 < x0 || y1 < y0)
         continue;

      if (active)
         color = core->menu_confirmed_button == core->menu_active_button ?
            deevee_core_rgb(255, 192, 32) : deevee_core_rgb(32, 255, 96);
      else
         color = deevee_core_rgb(64, 128, 255);
      label_color = active ? deevee_core_rgb(255, 255, 255) :
         deevee_core_rgb(160, 208, 255);
      thickness = active ? 2u : 1u;
      bracket = active ? 18u : 10u;
      if (bracket > (unsigned)(x1 - x0 + 1u))
         bracket = (unsigned)(x1 - x0 + 1u);
      if (bracket > (unsigned)(y1 - y0 + 1u))
         bracket = (unsigned)(y1 - y0 + 1u);

      for (y = y0; y <= y1; y++)
      {
         for (x = x0; x <= x1; x++)
         {
            bool top = y < y0 + thickness;
            bool bottom = y + thickness > y1;
            bool left = x < x0 + thickness;
            bool right = x + thickness > x1;
            bool near_left = x < x0 + bracket;
            bool near_right = x + bracket > x1;
            bool near_top = y < y0 + bracket;
            bool near_bottom = y + bracket > y1;
            bool corner = (top && (near_left || near_right)) ||
               (bottom && (near_left || near_right)) ||
               (left && (near_top || near_bottom)) ||
               (right && (near_top || near_bottom));

            if (corner)
               core->video.pixels[y * DEEVEE_VIDEO_WIDTH + x] = color;
         }
      }

      label_x = x0 >= 16u ? x0 - 14u :
         (x1 + 4u < DEEVEE_VIDEO_WIDTH ? x1 + 4u : x0 + 2u);
      label_y = y0 >= 12u ? y0 - 12u : y0 + 2u;
      draw_debug_button_number(core, button_number, label_x, label_y,
            label_color);
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

   fallback_ticks = frame_duration_with_repeat_pict(core, frame->repeat_pict);
   if (core->frame_queue_count)
   {
      struct deevee_decoded_video_frame *previous =
         &core->frame_queue[frame_queue_index(core->frame_queue_head,
               core->frame_queue_count - 1u)];

      if (previous->valid && previous->has_pts && frame->has_pts &&
            frame->pts > previous->pts)
      {
         previous->display_ticks = display_frame_duration_ticks(core,
               (uint64_t)(frame->pts - previous->pts));
         core->last_decoded_frame_duration_ticks = previous->display_ticks;
      }
      else if (previous->valid)
      {
         previous->display_ticks = frame_duration_with_repeat_pict(core,
               previous->repeat_pict);
         core->fallback_timing_frames++;
         core->last_decoded_frame_duration_ticks = previous->display_ticks;
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
   queued->repeat_pict = frame->repeat_pict;
   queued->width = frame->width;
   queued->height = frame->height;
   queued->pixel_format = frame->pixel_format;
   if (!core->last_decoded_frame_duration_ticks)
      core->last_decoded_frame_duration_ticks = fallback_ticks;
   core->last_decoded_repeat_pict = frame->repeat_pict;

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
   if (core->menu_base_pixels)
   {
      memcpy(core->menu_base_pixels, queued->pixels,
            DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT * sizeof(uint32_t));
      core->menu_base_pixels_valid = true;
   }
   core->menu_last_frame_width = queued->width;
   core->menu_last_frame_height = queued->height;
   core->menu_last_pixel_format = queued->pixel_format;
   core->menu_previous_frame_has_pts = core->menu_last_frame_has_pts;
   core->menu_previous_frame_pts = core->menu_last_frame_pts;
   core->menu_last_frame_has_pts = queued->has_pts;
   core->menu_last_frame_pts = queued->pts;
   core->display_frame_ticks_remaining =
      display_hold_ticks_from_duration(core, queued->display_ticks);
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
   if (!append_subpicture_packet(context->core, packet))
   {
      context->ok = false;
      return false;
   }

   if (packet->stream_id < 0xe0 || packet->stream_id > 0xef)
      return true;

   context->ok = append_menu_video_packet(context->core, packet);
   return context->ok;
}

static bool extract_audio_packet_callback(
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

   return true;
}

static void append_menu_audio_from_vob_path(struct deevee_core *core,
      struct deevee_disc *disc, const char *iso_path)
{
   struct packet_extract_context context;

   if (!core || !disc || !iso_path)
      return;

   context.core = core;
   context.ok = true;
   (void)deevee_dvd_walk_vob_packets(disc, iso_path,
         extract_audio_packet_callback, &context);
}

static void decode_available_subpictures(struct deevee_core *core)
{
   if (!core || !core->subpicture_chunk_count)
      return;

   if (!core->subpicture.opened)
   {
      if (deevee_subpicture_open_dvd(&core->subpicture) !=
            DEEVEE_SUBPICTURE_OK)
      {
         core->subpicture_decode_errors++;
         core->next_subpicture_chunk = core->subpicture_chunk_count;
         return;
      }
   }

   while (core->next_subpicture_chunk < core->subpicture_chunk_count)
   {
      const struct deevee_subpicture_payload_chunk *chunk =
         &core->subpicture_chunks[core->next_subpicture_chunk++];
      const uint8_t *chunk_payload =
         core->subpicture_payloads + chunk->offset;
      size_t chunk_size = chunk->size;
      size_t required_size;
      enum deevee_subpicture_status status =
         DEEVEE_SUBPICTURE_OK;

      if (!chunk_size)
         continue;

      if (!core->subpicture_assembly_size)
      {
         core->subpicture_assembly_has_pts = chunk->has_pts;
         core->subpicture_assembly_pts = chunk->pts;
      }

      required_size = core->subpicture_assembly_size + chunk_size;
      if (required_size > core->subpicture_assembly_capacity)
      {
         size_t new_capacity = core->subpicture_assembly_capacity ?
            core->subpicture_assembly_capacity * 2u : 4096u;
         uint8_t *new_assembly;

         while (new_capacity < required_size)
            new_capacity *= 2u;
         new_assembly = (uint8_t *)realloc(core->subpicture_assembly,
               new_capacity);
         if (!new_assembly)
         {
            core->subpicture_decode_errors++;
            core->subpicture_assembly_size = 0;
            core->subpicture_assembly_expected_size = 0;
            return;
         }

         core->subpicture_assembly = new_assembly;
         core->subpicture_assembly_capacity = new_capacity;
      }

      memcpy(core->subpicture_assembly + core->subpicture_assembly_size,
            chunk_payload, chunk_size);
      core->subpicture_assembly_size += chunk_size;

      if (!core->subpicture_assembly_expected_size &&
            core->subpicture_assembly_size >= 2u)
         core->subpicture_assembly_expected_size =
            read_be16(core->subpicture_assembly);

      while (core->subpicture_assembly_expected_size &&
            core->subpicture_assembly_size >=
               core->subpicture_assembly_expected_size)
      {
         size_t unit_size = core->subpicture_assembly_expected_size;
         size_t remaining = core->subpicture_assembly_size - unit_size;

         status = deevee_subpicture_decode_dvd_payload(&core->subpicture,
            core->subpicture_assembly, unit_size,
            core->subpicture_assembly_has_pts,
            core->subpicture_assembly_pts, &core->subpicture_frame);

         if (status != DEEVEE_SUBPICTURE_OK)
            core->subpicture_decode_errors++;
         else if (core->subpicture_frame.valid &&
               core->subpicture_frame.rect_count)
            core->subpicture_frame_ready = true;

         if (remaining)
         {
            memmove(core->subpicture_assembly,
                  core->subpicture_assembly + unit_size, remaining);
            core->subpicture_assembly_has_pts = false;
            core->subpicture_assembly_pts = 0;
         }

         core->subpicture_assembly_size = remaining;
         core->subpicture_assembly_expected_size = 0;
         if (core->subpicture_assembly_size >= 2u)
            core->subpicture_assembly_expected_size =
               read_be16(core->subpicture_assembly);
      }
   }
}

static uint8_t clamp_u8(unsigned value)
{
   return value > 255u ? 255u : (uint8_t)value;
}

static uint32_t blend_xrgb8888(uint32_t dst, uint32_t src)
{
   uint8_t alpha = (uint8_t)(src >> 24);
   unsigned inv_alpha = 255u - alpha;
   unsigned sr = (src >> 16) & 0xffu;
   unsigned sg = (src >> 8) & 0xffu;
   unsigned sb = src & 0xffu;
   unsigned dr = (dst >> 16) & 0xffu;
   unsigned dg = (dst >> 8) & 0xffu;
   unsigned db = dst & 0xffu;
   unsigned r;
   unsigned g;
   unsigned b;

   if (!alpha)
      return dst;
   if (alpha == 255u)
      return 0xff000000u | (src & 0x00ffffffu);

   r = (sr * alpha + dr * inv_alpha + 127u) / 255u;
   g = (sg * alpha + dg * inv_alpha + 127u) / 255u;
   b = (sb * alpha + db * inv_alpha + 127u) / 255u;
   return 0xff000000u | ((uint32_t)clamp_u8(r) << 16) |
      ((uint32_t)clamp_u8(g) << 8) | clamp_u8(b);
}

struct deevee_display_transform
{
   int x_offset;
   int y_offset;
   int x_num;
   int x_den;
   int y_num;
   int y_den;
};

static struct deevee_display_transform deevee_display_transform_for_core(
      const struct deevee_core *core)
{
   struct deevee_display_transform transform;

   transform.x_offset = 0;
   transform.y_offset = 0;
   transform.x_num = 1;
   transform.x_den = 1;
   transform.y_num = 1;
   transform.y_den = 1;

   (void)core;

   return transform;
}

static int transform_coordinate(int value, int offset, int num, int den)
{
   if (!den)
      return value;
   return offset + ((value - offset) * num) / den;
}

static int transform_dvd_x(const struct deevee_display_transform *transform,
      int x)
{
   return transform ? transform_coordinate(x, transform->x_offset,
      transform->x_num, transform->x_den) : x;
}

static int transform_dvd_y(const struct deevee_display_transform *transform,
      int y)
{
   return transform ? transform_coordinate(y, transform->y_offset,
      transform->y_num, transform->y_den) : y;
}

static bool button_ranges_overlap(uint16_t start_a, uint16_t end_a,
      uint16_t start_b, uint16_t end_b)
{
   return start_a <= end_b && start_b <= end_a;
}

static bool active_button_spu_region(const struct deevee_core *core,
      int *x0, int *y0, int *x1, int *y1)
{
   const struct deevee_dvd_menu_button *button;
   unsigned button_index;
   int active_x_center;
   int active_y_center;

   if (!core || !x0 || !y0 || !x1 || !y1 || !core->menu_active_button ||
         core->menu_active_button > core->menu_button_count)
      return false;

   if (core->has_active_highlight)
   {
      *x0 = (int)core->active_highlight_x_start;
      *x1 = (int)core->active_highlight_x_end;
      *y0 = (int)core->active_highlight_y_start;
      *y1 = (int)core->active_highlight_y_end;
      return *x1 >= *x0 && *y1 >= *y0;
   }

   button = &core->menu_buttons[core->menu_active_button - 1u];
   active_x_center = ((int)button->x_start + (int)button->x_end) / 2;
   active_y_center = ((int)button->y_start + (int)button->y_end) / 2;
   *x0 = 0;
   *x1 = (int)DEEVEE_VIDEO_WIDTH - 1;
   *y0 = 0;
   *y1 = (int)DEEVEE_VIDEO_HEIGHT - 1;

   for (button_index = 0; button_index < core->menu_button_count;
         button_index++)
   {
      const struct deevee_dvd_menu_button *other =
         &core->menu_buttons[button_index];
      int other_y_center;

      if (button_index + 1u == core->menu_active_button)
         continue;

      other_y_center = ((int)other->y_start + (int)other->y_end) / 2;
      if (other_y_center < active_y_center)
      {
         int boundary = (other_y_center + active_y_center) / 2;
         if (boundary > *y0)
            *y0 = boundary;
      }
      else if (other_y_center > active_y_center)
      {
         int boundary = (other_y_center + active_y_center) / 2;
         if (boundary < *y1)
            *y1 = boundary;
      }

      if (button_ranges_overlap(button->y_start, button->y_end,
               other->y_start, other->y_end))
      {
         int other_x_center =
            ((int)other->x_start + (int)other->x_end) / 2;

         if (other_x_center < active_x_center)
         {
            int boundary = (other_x_center + active_x_center) / 2;
            if (boundary > *x0)
               *x0 = boundary;
         }
         else if (other_x_center > active_x_center)
         {
            int boundary = (other_x_center + active_x_center) / 2;
            if (boundary < *x1)
               *x1 = boundary;
         }
      }
   }

   return *x1 >= *x0 && *y1 >= *y0;
}

static bool dvd_pixel_in_region(int x, int y, int x0, int y0, int x1, int y1)
{
   return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static bool subpicture_rect_pixel_is_present(
      const struct deevee_subpicture_rect *rect, size_t index)
{
   if (!rect)
      return false;
   if (rect->indexes)
      return rect->indexes[index] != 0;
   return rect->pixels && (rect->pixels[index] >> 24) != 0;
}

static bool build_active_button_spu_component_mask(
      const struct deevee_core *core,
      const struct deevee_subpicture_rect *rect, uint8_t **out_mask,
      int *out_x0, int *out_y0, int *out_x1, int *out_y1)
{
   int region_x0;
   int region_y0;
   int region_x1;
   int region_y1;
   size_t pixel_count;
   uint8_t *mask;
   uint8_t *visited;
   size_t *queue;
   size_t *component;
   int y;
   bool selected_any = false;

   if (out_mask)
      *out_mask = NULL;
   if (out_x0)
      *out_x0 = (int)DEEVEE_VIDEO_WIDTH - 1;
   if (out_y0)
      *out_y0 = (int)DEEVEE_VIDEO_HEIGHT - 1;
   if (out_x1)
      *out_x1 = 0;
   if (out_y1)
      *out_y1 = 0;
   if (!core || !rect || !out_mask || rect->width <= 0 || rect->height <= 0)
      return false;
   if (!active_button_spu_region(core, &region_x0, &region_y0, &region_x1,
            &region_y1))
      return false;

   pixel_count = (size_t)rect->width * (size_t)rect->height;
   mask = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
   if (!mask)
      return false;

   if (core->has_active_highlight)
   {
      for (y = 0; y < rect->height; y++)
      {
         int x;

         for (x = 0; x < rect->width; x++)
         {
            size_t index = (size_t)y * (size_t)rect->width + (size_t)x;
            int dvd_x = rect->x + x;
            int dvd_y = rect->y + y;

            if (!subpicture_rect_pixel_is_present(rect, index) ||
                  !dvd_pixel_in_region(dvd_x, dvd_y, region_x0, region_y0,
                     region_x1, region_y1))
               continue;

            mask[index] = 1;
            if (out_x0 && dvd_x < *out_x0)
               *out_x0 = dvd_x;
            if (out_y0 && dvd_y < *out_y0)
               *out_y0 = dvd_y;
            if (out_x1 && dvd_x > *out_x1)
               *out_x1 = dvd_x;
            if (out_y1 && dvd_y > *out_y1)
               *out_y1 = dvd_y;
            selected_any = true;
         }
      }

      if (!selected_any)
      {
         free(mask);
         return false;
      }

      *out_mask = mask;
      return true;
   }

   visited = (uint8_t *)calloc(pixel_count, sizeof(uint8_t));
   queue = (size_t *)malloc(pixel_count * sizeof(size_t));
   component = (size_t *)malloc(pixel_count * sizeof(size_t));
   if (!visited || !queue || !component)
   {
      free(mask);
      free(visited);
      free(queue);
      free(component);
      return false;
   }

   for (y = 0; y < rect->height; y++)
   {
      int x;

      for (x = 0; x < rect->width; x++)
      {
         size_t start = (size_t)y * (size_t)rect->width + (size_t)x;
         size_t head = 0;
         size_t tail = 0;
         size_t component_count = 0;
         int component_x0 = DEEVEE_VIDEO_WIDTH - 1;
         int component_y0 = DEEVEE_VIDEO_HEIGHT - 1;
         int component_x1 = 0;
         int component_y1 = 0;
         bool component_in_active_region = false;

         if (visited[start] || !subpicture_rect_pixel_is_present(rect, start))
            continue;

         visited[start] = 1;
         queue[tail++] = start;
         while (head < tail)
         {
            size_t current = queue[head++];
            int local_x = (int)(current % (size_t)rect->width);
            int local_y = (int)(current / (size_t)rect->width);
            int dvd_x = rect->x + local_x;
            int dvd_y = rect->y + local_y;

            component[component_count++] = current;
            if (dvd_x < component_x0)
               component_x0 = dvd_x;
            if (dvd_y < component_y0)
               component_y0 = dvd_y;
            if (dvd_x > component_x1)
               component_x1 = dvd_x;
            if (dvd_y > component_y1)
               component_y1 = dvd_y;

            if (local_x > 0)
            {
               size_t neighbor = current - 1u;
               if (!visited[neighbor] &&
                     subpicture_rect_pixel_is_present(rect, neighbor))
               {
                  visited[neighbor] = 1;
                  queue[tail++] = neighbor;
               }
            }
            if (local_x + 1 < rect->width)
            {
               size_t neighbor = current + 1u;
               if (!visited[neighbor] &&
                     subpicture_rect_pixel_is_present(rect, neighbor))
               {
                  visited[neighbor] = 1;
                  queue[tail++] = neighbor;
               }
            }
            if (local_y > 0)
            {
               size_t neighbor = current - (size_t)rect->width;
               if (!visited[neighbor] &&
                     subpicture_rect_pixel_is_present(rect, neighbor))
               {
                  visited[neighbor] = 1;
                  queue[tail++] = neighbor;
               }
            }
            if (local_y + 1 < rect->height)
            {
               size_t neighbor = current + (size_t)rect->width;
               if (!visited[neighbor] &&
                     subpicture_rect_pixel_is_present(rect, neighbor))
               {
                  visited[neighbor] = 1;
                  queue[tail++] = neighbor;
               }
            }
         }

         component_in_active_region = dvd_pixel_in_region(
               (component_x0 + component_x1) / 2,
               (component_y0 + component_y1) / 2,
               region_x0, region_y0, region_x1, region_y1);

         if (component_in_active_region)
         {
            size_t i;

            for (i = 0; i < component_count; i++)
            {
               int local_x = (int)(component[i] % (size_t)rect->width);
               int local_y = (int)(component[i] / (size_t)rect->width);
               int dvd_x = rect->x + local_x;
               int dvd_y = rect->y + local_y;

               mask[component[i]] = 1;
               if (out_x0 && dvd_x < *out_x0)
                  *out_x0 = dvd_x;
               if (out_y0 && dvd_y < *out_y0)
                  *out_y0 = dvd_y;
               if (out_x1 && dvd_x > *out_x1)
                  *out_x1 = dvd_x;
               if (out_y1 && dvd_y > *out_y1)
                  *out_y1 = dvd_y;
            }
            selected_any = true;
         }
      }
   }

   free(visited);
   free(queue);
   free(component);

   if (!selected_any)
   {
      free(mask);
      return false;
   }

   *out_mask = mask;
   return true;
}

static int transform_spu_region_x_for_active_button(
      const struct deevee_core *core, int mask_x0, int x)
{
   const struct deevee_dvd_menu_button *button;

   if (core && core->has_active_highlight)
      return x;

   if (!core || !core->menu_active_button ||
         core->menu_active_button > core->menu_button_count)
      return x;

   button = &core->menu_buttons[core->menu_active_button - 1u];
   if (mask_x0 >= (int)button->x_start - 12)
      return x;

   return (int)button->x_start + 8 + ((x - mask_x0) * 3) / 4;
}

static uint32_t dvd_clut_ycrcb_to_argb(uint32_t clut, uint8_t alpha)
{
   int y = (int)((clut >> 16) & 0xffu);
   int cr = (int)((clut >> 8) & 0xffu) - 128;
   int cb = (int)(clut & 0xffu) - 128;
   int r = y + ((91881 * cr) >> 16);
   int g = y - ((22554 * cb + 46802 * cr) >> 16);
   int b = y + ((116130 * cb) >> 16);

   return deevee_core_argb(alpha, clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

static uint32_t subpicture_highlight_pixel(const struct deevee_core *core,
      uint8_t spu_index, uint32_t decoded_pixel)
{
   unsigned table_index;
   uint32_t color_entry;
   uint8_t clut_index;
   uint8_t alpha;

   if (!spu_index)
      return 0;

   if (!core || !core->has_subpicture_clut)
      return decoded_pixel;

   if (core->has_active_highlight)
      color_entry = core->active_highlight_palette;
   else
   {
      if (!core->has_select_color_table || !core->has_active_button_color_table)
         return decoded_pixel;

      table_index = core->active_button_color_table;
      if (table_index)
         table_index--;
      if (table_index >= 3u)
         table_index = 0;
      color_entry = core->select_color_table[table_index];
   }
   clut_index = (uint8_t)((color_entry >> (16u + (unsigned)spu_index * 4u)) &
         0x0fu);
   alpha = (uint8_t)(((color_entry >> ((unsigned)spu_index * 4u)) & 0x0fu) *
         17u);
   if (!alpha)
      return 0;

   return dvd_clut_ycrcb_to_argb(core->subpicture_clut[clut_index], alpha);
}

static bool composite_subpicture_highlight(struct deevee_core *core)
{
   struct deevee_display_transform transform;
   unsigned rect_index;
   bool drew = false;

   if (!core || !core->video.pixels || core->playback_is_title)
      return false;

   decode_available_subpictures(core);
   if (!core->subpicture_frame_ready || !core->subpicture_frame.valid ||
         !core->subpicture_frame.rect_count)
      return false;

   transform = deevee_display_transform_for_core(core);
   if (core->menu_base_pixels && core->menu_base_pixels_valid)
      memcpy(core->video.pixels, core->menu_base_pixels,
            DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT * sizeof(uint32_t));

   for (rect_index = 0; rect_index < core->subpicture_frame.rect_count;
         rect_index++)
   {
      const struct deevee_subpicture_rect *rect =
         &core->subpicture_frame.rects[rect_index];
      uint8_t *active_mask = NULL;
      int mask_x0 = 0;
      int mask_y0 = 0;
      int mask_x1 = 0;
      int mask_y1 = 0;
      int y;

      if (!rect->pixels || rect->width <= 0 || rect->height <= 0)
         continue;
      if (!build_active_button_spu_component_mask(core, rect, &active_mask,
               &mask_x0, &mask_y0, &mask_x1, &mask_y1))
         continue;

      for (y = 0; y < rect->height; y++)
      {
         int src_y = rect->y + y;
         int dst_y;
         int x;

         if (src_y < 0 || src_y >= (int)DEEVEE_VIDEO_HEIGHT)
            continue;

         for (x = 0; x < rect->width; x++)
         {
            int src_x = rect->x + x;
            int dst_x;
            uint32_t src;
            size_t dst_index;
            uint8_t spu_index = rect->indexes ?
               rect->indexes[(size_t)y * (size_t)rect->width + (size_t)x] : 0;

            if (src_x < 0 || src_x >= (int)DEEVEE_VIDEO_WIDTH ||
                  !active_mask[(size_t)y * (size_t)rect->width + (size_t)x])
               continue;

            src = rect->pixels[(size_t)y * (size_t)rect->width + (size_t)x];
            src = subpicture_highlight_pixel(core, spu_index, src);
            if (!(src >> 24))
               continue;

            dst_x = transform_spu_region_x_for_active_button(core, mask_x0,
                  transform_dvd_x(&transform, src_x));
            dst_y = transform_dvd_y(&transform, src_y);
            if (dst_x < 0 || dst_x >= (int)DEEVEE_VIDEO_WIDTH ||
                  dst_y < 0 || dst_y >= (int)DEEVEE_VIDEO_HEIGHT)
               continue;

            dst_index = (size_t)dst_y * DEEVEE_VIDEO_WIDTH + (size_t)dst_x;
            core->video.pixels[dst_index] =
               blend_xrgb8888(core->video.pixels[dst_index], src);
            drew = true;
         }
      }
      free(active_mask);
   }

   return drew;
}

static bool composite_subpicture_overlay(struct deevee_core *core)
{
   struct deevee_display_transform transform;
   unsigned rect_index;
   bool drew = false;

   if (!core || !core->video.pixels || !core->playback_is_title ||
         !core->subpicture_visible)
      return false;

   decode_available_subpictures(core);
   if (!core->subpicture_frame_ready || !core->subpicture_frame.valid ||
         !core->subpicture_frame.rect_count)
      return false;

   transform = deevee_display_transform_for_core(core);
   for (rect_index = 0; rect_index < core->subpicture_frame.rect_count;
         rect_index++)
   {
      const struct deevee_subpicture_rect *rect =
         &core->subpicture_frame.rects[rect_index];
      int y;

      if (!rect->pixels || rect->width <= 0 || rect->height <= 0)
         continue;

      for (y = 0; y < rect->height; y++)
      {
         int src_y = rect->y + y;
         int dst_y;
         int x;

         if (src_y < 0 || src_y >= (int)DEEVEE_VIDEO_HEIGHT)
            continue;

         dst_y = transform_dvd_y(&transform, src_y);
         if (dst_y < 0 || dst_y >= (int)DEEVEE_VIDEO_HEIGHT)
            continue;

         for (x = 0; x < rect->width; x++)
         {
            int src_x = rect->x + x;
            int dst_x;
            uint32_t src;
            size_t dst_index;

            if (src_x < 0 || src_x >= (int)DEEVEE_VIDEO_WIDTH)
               continue;

            src = rect->pixels[(size_t)y * (size_t)rect->width + (size_t)x];
            if (!(src >> 24))
               continue;

            dst_x = transform_dvd_x(&transform, src_x);
            if (dst_x < 0 || dst_x >= (int)DEEVEE_VIDEO_WIDTH)
               continue;

            dst_index = (size_t)dst_y * DEEVEE_VIDEO_WIDTH + (size_t)dst_x;
            core->video.pixels[dst_index] =
               blend_xrgb8888(core->video.pixels[dst_index], src);
            drew = true;
         }
      }
   }

   return drew;
}

static void sync_dvdnav_buttons(struct deevee_core *core)
{
   uint8_t button_count = 0;
   uint8_t active_button = 0;
   uint32_t select_color_table[3];
   uint32_t subpicture_clut[16];
   bool has_select_color_table = false;
   bool has_subpicture_clut = false;
   struct deevee_dvdnav_highlight highlight;

   if (!core || !core->dvdnav_active)
      return;

   update_dvdnav_position(core);
   core->has_active_highlight = false;
   if (deevee_dvdnav_read_buttons(&core->dvdnav, core->menu_buttons,
            &button_count, &active_button, select_color_table,
            &has_select_color_table, subpicture_clut, &has_subpicture_clut))
   {
      core->menu_button_count = button_count;
      core->menu_active_button = active_button;
      if (core->menu_active_button > core->menu_button_count)
         core->menu_active_button = core->menu_button_count;
      if (core->menu_active_button)
      {
         core->active_button_color_table =
            core->menu_buttons[core->menu_active_button - 1u].color_table;
         core->has_active_button_color_table = true;
      }
      if (has_select_color_table)
      {
         memcpy(core->select_color_table, select_color_table,
               sizeof(core->select_color_table));
         core->has_select_color_table = true;
      }
      if (has_subpicture_clut)
      {
         memcpy(core->subpicture_clut, subpicture_clut,
               sizeof(core->subpicture_clut));
         core->has_subpicture_clut = true;
      }
      if (deevee_dvdnav_read_highlight(&core->dvdnav, &highlight, false))
      {
         core->active_highlight_x_start = highlight.x_start;
         core->active_highlight_x_end = highlight.x_end;
         core->active_highlight_y_start = highlight.y_start;
         core->active_highlight_y_end = highlight.y_end;
         core->active_highlight_palette = highlight.palette;
         core->has_active_highlight = true;
      }
   }
}

static bool append_dvdnav_events(struct deevee_core *core,
      unsigned max_events, size_t min_new_video_chunks,
      size_t min_new_audio_chunks)
{
   size_t start_video_chunks;
   size_t start_audio_chunks;
   unsigned i;

   if (!core || !core->dvdnav_active)
      return false;

   start_video_chunks = core->menu_video_chunk_count;
   start_audio_chunks = core->audio_chunk_count;
   for (i = 0; i < max_events &&
         (core->menu_video_chunk_count <
             start_video_chunks + min_new_video_chunks ||
          core->audio_chunk_count <
             start_audio_chunks + min_new_audio_chunks); i++)
   {
      struct deevee_dvdnav_event event;
      enum deevee_dvdnav_status status;
      const char *name;

      status = deevee_dvdnav_next(&core->dvdnav, &event);
      if (status != DEEVEE_DVDNAV_OK)
         return core->menu_video_chunk_count > start_video_chunks ||
            core->audio_chunk_count > start_audio_chunks;

      name = deevee_dvdnav_event_name(event.event);
      if (strcmp(name, "block") == 0 || strcmp(name, "nav") == 0)
      {
         struct packet_extract_context context;

         context.core = core;
         context.ok = true;
         if (deevee_dvd_walk_packets_in_buffer(event.data,
                  (size_t)event.length, extract_video_packet_callback,
                  &context) != DEEVEE_DVD_OK || !context.ok)
            return false;
      }
      else if (strcmp(name, "stop") == 0)
      {
         core->dvdnav_active = false;
         update_dvdnav_position(core);
         return core->menu_video_chunk_count > start_video_chunks ||
            core->audio_chunk_count > start_audio_chunks;
      }
      else if (strcmp(name, "highlight") == 0)
         sync_dvdnav_buttons(core);
      else if (strcmp(name, "audio_stream") == 0)
      {
         struct deevee_dvdnav_audio_stream_change change;
         if (deevee_dvdnav_read_audio_stream_change(&event, &change))
            apply_audio_stream_change(core, change.physical, change.logical);
      }
      else if (strcmp(name, "spu_stream") == 0)
      {
         struct deevee_dvdnav_spu_stream_change change;
         if (deevee_dvdnav_read_spu_stream_change(&event, &change))
         {
            int physical = change.physical_wide >= 0 ?
               change.physical_wide : change.physical_letterbox;
            if (physical < 0)
               physical = change.physical_pan_scan;
            apply_subpicture_stream_change(core, physical, change.logical);
         }
      }

      if (strcmp(name, "nav") == 0)
         sync_dvdnav_buttons(core);

      (void)deevee_dvdnav_ack_event(&core->dvdnav, event.event);
   }

   return core->menu_video_chunk_count > start_video_chunks ||
      core->audio_chunk_count > start_audio_chunks;
}

static bool reset_dvdnav_decode_after_control(struct deevee_core *core)
{
   if (!core || !core->dvdnav_active)
      return false;

   clear_stream_buffers_keep_dvdnav(core);
   if (core->audio_stream_forced && core->forced_audio_physical_stream >= 0)
      (void)deevee_dvdnav_set_active_stream(&core->dvdnav, true,
            core->forced_audio_physical_stream);
   if (core->subpicture_stream_forced &&
         core->forced_subpicture_physical_stream >= 0)
      (void)deevee_dvdnav_set_active_stream(&core->dvdnav, false,
            core->forced_subpicture_physical_stream);
   (void)deevee_dvdnav_set_spu_visible(&core->dvdnav,
         core->subpicture_visible);
   if (!append_dvdnav_events(core, 32768u, 2048u, 0u) ||
         !core->menu_video_chunk_count)
      return false;

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      return false;

   core->next_menu_video_chunk = 0;
   core->menu_playback_active = true;
   core->playback_paused = false;
   update_dvdnav_position(core);
   return true;
}

static bool prepare_dvdnav_title_playback(struct deevee_core *core,
      unsigned title_number, unsigned part_number)
{
   enum deevee_dvdnav_status status;
   bool opened_here = false;

   if (!core || !title_number || !deevee_dvdnav_available() ||
         !content_is_disc_image(&core->content))
      return false;

   if (!core->dvdnav_active)
   {
      status = deevee_dvdnav_open(&core->dvdnav, &core->content);
      if (status != DEEVEE_DVDNAV_OK)
         return false;
      core->dvdnav_active = true;
      opened_here = true;
   }

   if (deevee_dvdnav_play_title_part(&core->dvdnav, (int)title_number,
            (int)(part_number ? part_number : 1)) &&
         reset_dvdnav_decode_after_control(core))
      return true;

   if (opened_here)
   {
      deevee_dvdnav_close(&core->dvdnav);
      core->dvdnav_active = false;
      update_dvdnav_position(core);
   }
   return false;
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

   append_menu_audio_from_vob_path(core, &disc, "/VIDEO_TS/VIDEO_TS.VOB");
   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 6u);
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
      core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_VMG;
   }
   return prepared;
}

static bool prepare_menu_playback_from_vob_path(struct deevee_core *core,
      const struct deevee_content_info *content, const char *iso_path)
{
   struct deevee_disc disc;
   struct packet_extract_context extract_context;
   bool prepared = false;

   if (!core || !content_is_disc_image(content) || !iso_path)
      return false;

   clear_menu_video(core);
   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, content) != DEEVEE_DISC_OK)
      return false;

   extract_context.core = core;
   extract_context.ok = true;
   if (deevee_dvd_walk_vob_packets(&disc, iso_path,
            extract_video_packet_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   {
      struct deevee_dvd_menu_render_probe render_probe;

      memset(&render_probe, 0, sizeof(render_probe));
      if (deevee_dvd_probe_vob_menu_render_streams(&disc, iso_path,
               &render_probe) == DEEVEE_DVD_OK)
         set_menu_buttons_from_probe(core, &render_probe);
   }

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 6u);
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
      core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_NONE;
   }
   return prepared;
}

static bool prepare_menu_playback_from_vts_pgc(struct deevee_core *core,
      const struct deevee_content_info *content, unsigned vts,
      unsigned pgc_index)
{
   struct deevee_disc disc;
   struct packet_extract_context extract_context;
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
   if (deevee_dvd_walk_vts_menu_pgc_packets(&disc, vts, pgc_index,
            extract_video_packet_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   if (deevee_dvd_probe_vts_menu_pgc_render_streams(&disc, vts, pgc_index,
            &render_probe) == DEEVEE_DVD_OK)
      set_menu_buttons_from_probe(core, &render_probe);

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 6u);
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
      core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_VTS;
   }
   return prepared;
}

static bool prepare_menu_playback_from_vmgm_pgc(struct deevee_core *core,
      const struct deevee_content_info *content, unsigned pgc_index)
{
   struct deevee_disc disc;
   struct packet_extract_context extract_context;
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
   if (deevee_dvd_walk_vmgm_menu_pgc_packets(&disc, pgc_index,
            extract_video_packet_callback, &extract_context) != DEEVEE_DVD_OK ||
         !extract_context.ok || !core->menu_video_chunk_count)
      goto end_disc;

   if (deevee_dvd_probe_vmgm_menu_pgc_render_streams(&disc, pgc_index,
            &render_probe) == DEEVEE_DVD_OK)
      set_menu_buttons_from_probe(core, &render_probe);

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
      goto end_disc;

   core->next_menu_video_chunk = 0;
   decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 6u);
   prepared = true;

end_disc:
   deevee_disc_close(&disc);
   if (!prepared)
      clear_menu_video(core);
   else
   {
      snprintf(core->menu_playback_source,
            sizeof(core->menu_playback_source), "VMGM_PGC_%02u",
            pgc_index + 1u);
      core->menu_playback_source[sizeof(core->menu_playback_source) - 1] =
         '\0';
      core->menu_current_vts = 0;
      core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_VMG;
   }
   return prepared;
}

static bool prepare_title_playback_from_target(struct deevee_core *core,
      const struct deevee_dvd_playback_target *target)
{
   struct deevee_disc disc;
   struct deevee_dvd_title_pgc title_pgc;
   struct packet_extract_context extract_context;
   bool prepared = false;
   bool cleared_video = false;

   if (!core || !target || target->type != DEEVEE_DVD_PLAYBACK_TARGET_TITLE ||
         !content_is_disc_image(&core->content))
      return false;

   if (target->title_number &&
         prepare_dvdnav_title_playback(core, target->title_number,
            target->ptt_number ? target->ptt_number : 1))
   {
      core->playback_is_title = true;
      core->menu_current_vts = target->vts_number;
      core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_NONE;
      snprintf(core->menu_playback_source, sizeof(core->menu_playback_source),
            "DVDNAV_TITLE_%02u_PART_%02u", target->title_number,
            target->ptt_number ? target->ptt_number : 1);
      core->menu_playback_source[sizeof(core->menu_playback_source) - 1] =
         '\0';
      return true;
   }

   deevee_disc_init(&disc);
   if (deevee_disc_open(&disc, &core->content) != DEEVEE_DISC_OK)
      return false;

   memset(&title_pgc, 0, sizeof(title_pgc));

   if (deevee_dvd_resolve_title_pgc(&disc, target, &title_pgc) !=
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
   core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_NONE;
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

static bool prepare_dvdnav_playback(struct deevee_core *core,
      const struct deevee_content_info *content)
{
   enum deevee_dvdnav_status status;

   if (!core || !content_is_disc_image(content) || !deevee_dvdnav_available())
      return false;

   clear_menu_video(core);
   status = deevee_dvdnav_open(&core->dvdnav, content);
   if (status != DEEVEE_DVDNAV_OK)
      return false;

   core->dvdnav_active = true;
   (void)deevee_dvdnav_menu_call_root(&core->dvdnav);
   if (!append_dvdnav_events(core, 4096u, 512u, 0u) ||
         !core->menu_video_chunk_count)
   {
      clear_menu_video(core);
      return false;
   }

   detect_menu_frame_repeat(core);
   if (!open_menu_decoder(core))
   {
      clear_menu_video(core);
      return false;
   }

   core->next_menu_video_chunk = 0;
   core->playback_is_title = false;
   core->menu_current_vts = 0;
   core->menu_domain = DEEVEE_DVD_MENU_DOMAIN_NONE;
   snprintf(core->menu_playback_source, sizeof(core->menu_playback_source),
         "DVDNAV");
   update_dvdnav_position(core);
   return true;
}

static bool prepare_any_menu_playback(struct deevee_core *core,
      const struct deevee_content_info *content)
{
   unsigned vts;
   char path[32];

   if (prepare_dvdnav_playback(core, content) &&
         keep_prepared_menu_if_decodable(core))
      return true;

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

   if (core->menu_at_end)
      return core->frame_queue_count > 0;

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

         if (core->dvdnav_active &&
               append_dvdnav_events(core, 512u, target_count, 0u))
            continue;

         (void)deevee_decoder_flush_mpeg2(&core->decoder, &frame_probe);
         core->menu_frames_decoded +=
            frame_probe.frames_decoded - frames_before_flush;
         if (core->frame_queue_count >= target_count)
            break;
         if (!core->menu_loop_enabled)
         {
            core->menu_at_end = true;
            break;
         }
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

   if (core->menu_at_end)
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
         if (core->dvdnav_active)
         {
            size_t audio_chunks_before = core->audio_chunk_count;

            if (!append_dvdnav_events(core, 512u, 0u, 1u) ||
                  core->audio_chunk_count <= audio_chunks_before)
               break;
         }
         else if (!core->playback_is_title && core->menu_loop_enabled)
         {
            deevee_audio_deinit(&core->audio);
            deevee_audio_init(&core->audio);
            core->next_audio_chunk = 0;
         }
         else
            break;
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
   deevee_dvdnav_init(&core->dvdnav);
   deevee_subpicture_init(&core->subpicture);

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
   bool drew_subpicture_highlight = false;

   if (!core || !video || !audio)
      return;

   if (core->loaded)
      label = deevee_content_type_name(core->content.type);

   update_menu_button_selection(core);
   update_dvdnav_position(core);

   if (core->menu_playback_active)
   {
      if (core->playback_paused)
         core->repeated_frames++;
      else if (core->display_frame_ticks_remaining)
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

   drew_subpicture_highlight = composite_subpicture_overlay(core);
   if (!drew_subpicture_highlight)
      drew_subpicture_highlight = composite_subpicture_highlight(core);
   if (!drew_subpicture_highlight && should_draw_menu_overlay(core))
      draw_menu_button_overlay(core);

   video->pixels = core->video.pixels;
   video->width = DEEVEE_VIDEO_WIDTH;
   video->height = DEEVEE_VIDEO_HEIGHT;
   video->pitch = core->video.pitch;
   if (core->playback_paused)
   {
      audio->samples = deevee_audio_silence(&core->audio, &audio->frames);
      audio->frames = 0;
   }
   else
   {
      decode_audio_until_buffered(core, DEEVEE_AUDIO_FRAMES_PER_RUN * 3u);
      audio->samples = deevee_audio_read(&core->audio, &audio->frames);
   }

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

unsigned deevee_core_active_audio_stream(const struct deevee_core *core)
{
   return core && core->has_active_audio_stream ? core->active_audio_stream : 0;
}

unsigned deevee_core_audio_stream_count(const struct deevee_core *core)
{
   return core ? core->audio_stream_count : 0;
}

int deevee_core_active_audio_logical_stream(const struct deevee_core *core)
{
   return core ? core->active_audio_logical_stream : -1;
}

int deevee_core_active_audio_physical_stream(const struct deevee_core *core)
{
   return core ? core->active_audio_physical_stream : -1;
}

const char *deevee_core_audio_stream_label(const struct deevee_core *core,
      unsigned index)
{
   if (!core || index >= core->audio_stream_count ||
         !core->audio_streams[index].label[0])
      return "";

   return core->audio_streams[index].label;
}

bool deevee_core_set_audio_track(struct deevee_core *core, int physical_stream)
{
   if (!core)
      return false;

   if (physical_stream < 0)
   {
      core->audio_stream_forced = false;
      refresh_track_inventory(core);
      return true;
   }

   if (physical_stream >= (int)DEEVEE_MAX_AUDIO_STREAMS)
      return false;
   if (!core->audio_stream_count ||
         physical_stream >= (int)core->audio_stream_count)
      return false;
   if (core->audio_stream_forced &&
         core->forced_audio_physical_stream == physical_stream)
      return true;

   core->audio_stream_forced = true;
   core->forced_audio_physical_stream = physical_stream;
   core->active_audio_physical_stream = physical_stream;
   core->active_audio_logical_stream = physical_stream;
   core->active_audio_stream = (uint8_t)(0x80u + (unsigned)physical_stream);
   core->has_active_audio_stream = true;
   if (core->dvdnav_active)
      (void)deevee_dvdnav_set_active_stream(&core->dvdnav, true,
            physical_stream);
   reset_audio_stream_buffers(core);
   core->track_options_dirty = true;
   return true;
}

uint64_t deevee_core_subpicture_payload_count(const struct deevee_core *core)
{
   return core ? (uint64_t)core->subpicture_chunk_count : 0;
}

uint64_t deevee_core_subpicture_decode_errors(const struct deevee_core *core)
{
   return core ? core->subpicture_decode_errors : 0;
}

bool deevee_core_subpicture_frame_ready(const struct deevee_core *core)
{
   return core && core->subpicture_frame_ready;
}

unsigned deevee_core_subpicture_rect_count(const struct deevee_core *core)
{
   return core && core->subpicture_frame.valid ?
      core->subpicture_frame.rect_count : 0;
}

unsigned deevee_core_active_subpicture_stream(const struct deevee_core *core)
{
   return core && core->has_active_subpicture_stream ?
      core->active_subpicture_stream : 0;
}

unsigned deevee_core_subpicture_stream_count(const struct deevee_core *core)
{
   return core ? core->subpicture_stream_count : 0;
}

int deevee_core_active_subpicture_logical_stream(const struct deevee_core *core)
{
   return core ? core->active_subpicture_logical_stream : -1;
}

int deevee_core_active_subpicture_physical_stream(const struct deevee_core *core)
{
   return core ? core->active_subpicture_physical_stream : -1;
}

bool deevee_core_subpicture_visible(const struct deevee_core *core)
{
   return core && core->subpicture_visible;
}

const char *deevee_core_subpicture_stream_label(const struct deevee_core *core,
      unsigned index)
{
   if (!core || index >= core->subpicture_stream_count ||
         !core->subpicture_streams[index].label[0])
      return "";

   return core->subpicture_streams[index].label;
}

bool deevee_core_set_subtitle_track(struct deevee_core *core,
      int physical_stream, bool visible)
{
   if (!core)
      return false;

   core->subpicture_visible = visible;
   if (core->dvdnav_active)
      (void)deevee_dvdnav_set_spu_visible(&core->dvdnav, visible);

   if (physical_stream < 0)
   {
      core->subpicture_stream_forced = false;
      refresh_track_inventory(core);
      reset_subpicture_stream_buffers(core);
      core->track_options_dirty = true;
      return true;
   }

   if (physical_stream >= (int)DEEVEE_MAX_SUBTITLE_STREAMS)
      return false;
   if (!core->subpicture_stream_count ||
         physical_stream >= (int)core->subpicture_stream_count)
      return false;
   if (core->subpicture_stream_forced &&
         core->forced_subpicture_physical_stream == physical_stream &&
         core->subpicture_visible == visible)
      return true;

   core->subpicture_stream_forced = true;
   core->forced_subpicture_physical_stream = physical_stream;
   core->active_subpicture_physical_stream = physical_stream;
   core->active_subpicture_logical_stream = physical_stream;
   core->active_subpicture_stream =
      (uint8_t)(0x20u + (unsigned)physical_stream);
   core->has_active_subpicture_stream = true;
   if (core->dvdnav_active)
      (void)deevee_dvdnav_set_active_stream(&core->dvdnav, false,
            physical_stream);
   reset_subpicture_stream_buffers(core);
   core->track_options_dirty = true;
   return true;
}

bool deevee_core_track_options_dirty(const struct deevee_core *core)
{
   return core && core->track_options_dirty;
}

void deevee_core_clear_track_options_dirty(struct deevee_core *core)
{
   if (core)
      core->track_options_dirty = false;
}

unsigned deevee_core_video_frame_rate_code(const struct deevee_core *core)
{
   return core ? core->video_frame_rate_code : 0;
}

unsigned deevee_core_video_frame_duration_ticks(const struct deevee_core *core)
{
   return core ? core->video_frame_duration_ticks : 0;
}

unsigned deevee_core_last_decoded_frame_duration_ticks(
      const struct deevee_core *core)
{
   return core ? core->last_decoded_frame_duration_ticks : 0;
}

int deevee_core_last_decoded_repeat_pict(const struct deevee_core *core)
{
   return core ? core->last_decoded_repeat_pict : 0;
}

bool deevee_core_dvdnav_active(const struct deevee_core *core)
{
   return core && core->dvdnav_active;
}

bool deevee_core_dvdnav_has_position(const struct deevee_core *core)
{
   return core && core->dvdnav_has_position;
}

int deevee_core_dvdnav_title(const struct deevee_core *core)
{
   return core ? core->dvdnav_title : 0;
}

int deevee_core_dvdnav_part(const struct deevee_core *core)
{
   return core ? core->dvdnav_part : 0;
}

int deevee_core_dvdnav_parts(const struct deevee_core *core)
{
   return core ? core->dvdnav_parts : 0;
}

int64_t deevee_core_dvdnav_time_ticks(const struct deevee_core *core)
{
   return core ? core->dvdnav_time_ticks : -1;
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

const char *deevee_core_menu_command_status(const struct deevee_core *core)
{
   if (!core || !core->menu_command_status[0])
      return "none";

   return core->menu_command_status;
}
