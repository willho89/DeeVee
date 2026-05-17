#include "deevee_core.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DEEVEE_MIN_MENU_VIDEO_PAYLOAD_BYTES 65536u
#define DEEVEE_MENU_VISIBILITY_SAMPLE_FRAMES 30u
#define DEEVEE_MENU_FRAME_REPEAT 2u

struct payload_extract_context
{
   struct deevee_core *core;
   bool ok;
};

static bool decode_next_menu_frame(struct deevee_core *core);
static bool open_menu_decoder(struct deevee_core *core);

static void clear_menu_video(struct deevee_core *core)
{
   if (!core)
      return;

   deevee_decoder_deinit(&core->decoder);
   free(core->menu_video_payloads);
   free(core->menu_video_chunks);
   core->menu_video_payloads = NULL;
   core->menu_video_payload_size = 0;
   core->menu_video_payload_capacity = 0;
   core->menu_video_chunks = NULL;
   core->menu_video_chunk_count = 0;
   core->menu_video_chunk_capacity = 0;
   core->next_menu_video_chunk = 0;
   core->menu_frame_hold = 0;
   core->menu_playback_active = false;
   core->menu_playback_source[0] = '\0';
   core->menu_packets_sent = 0;
   core->menu_frames_decoded = 0;
   core->menu_last_frame_width = 0;
   core->menu_last_frame_height = 0;
   core->menu_last_pixel_format = 0;
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
   core->menu_video_payload_size += payload_size;
   core->menu_video_chunk_count++;
   return true;
}

static bool reset_menu_decoder_position(struct deevee_core *core)
{
   if (!core || !core->menu_video_chunk_count)
      return false;

   if (!open_menu_decoder(core))
      return false;

   core->next_menu_video_chunk = 0;
   core->menu_frame_hold = 0;
   core->menu_packets_sent = 0;
   core->menu_frames_decoded = 0;
   core->menu_last_frame_width = 0;
   core->menu_last_frame_height = 0;
   core->menu_last_pixel_format = 0;
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
            core->video.pixels, DEEVEE_VIDEO_WIDTH, DEEVEE_VIDEO_HEIGHT,
            core->video.pitch) != DEEVEE_DECODER_OK)
      return false;

   return true;
}

static bool prepare_menu_playback(struct deevee_core *core,
      const struct deevee_content_info *content)
{
   struct deevee_disc disc;
   struct deevee_dvd_info dvd_info;
   struct payload_extract_context extract_context;
   bool prepared = false;

   if (!core || !content || content->type != DEEVEE_CONTENT_ISO)
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

   if (!core || !content || !iso_path || content->type != DEEVEE_CONTENT_ISO)
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

static bool decode_next_menu_frame(struct deevee_core *core)
{
   struct deevee_decoder_frame_probe frame_probe;
   size_t attempts = 0;

   if (!core || !core->menu_video_chunk_count)
      return false;

   memset(&frame_probe, 0, sizeof(frame_probe));

   while (!frame_probe.got_frame &&
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
         if (frame_probe.got_frame)
         {
            core->menu_frames_decoded +=
               frame_probe.frames_decoded - frames_before_flush;
            if (frame_probe.frames_decoded > frames_before_flush)
            {
               core->menu_last_frame_width = frame_probe.width;
               core->menu_last_frame_height = frame_probe.height;
               core->menu_last_pixel_format = frame_probe.pixel_format;
            }
            break;
         }
         if (!open_menu_decoder(core))
            return false;
         core->next_menu_video_chunk = 0;
      }

      chunk = &core->menu_video_chunks[core->next_menu_video_chunk++];
      payload = core->menu_video_payloads + chunk->offset;
      if (deevee_decoder_decode_mpeg2_payload(&core->decoder, payload,
               chunk->size, &frame_probe) != DEEVEE_DECODER_OK)
      {
         if (!open_menu_decoder(core))
            return false;
         attempts++;
         continue;
      }

      core->menu_packets_sent += frame_probe.packets_sent - packets_before;
      core->menu_frames_decoded += frame_probe.frames_decoded - frames_before;
      if (frame_probe.got_frame && frame_probe.frames_decoded > frames_before)
      {
         core->menu_last_frame_width = frame_probe.width;
         core->menu_last_frame_height = frame_probe.height;
         core->menu_last_pixel_format = frame_probe.pixel_format;
      }

      attempts++;
   }

   return frame_probe.got_frame;
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

   core->initialized = true;
   return true;
}

void deevee_core_deinit(struct deevee_core *core)
{
   if (!core)
      return;

   clear_menu_video(core);
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

   if (core->menu_playback_active)
   {
      if (core->menu_frame_hold)
         core->menu_frame_hold--;
      else if (!decode_next_menu_frame(core))
      {
         core->menu_playback_active = false;
         deevee_video_render_placeholder(&core->video, core->frame_count,
               label);
      }
      else
         core->menu_frame_hold = DEEVEE_MENU_FRAME_REPEAT - 1u;
   }
   else
      deevee_video_render_placeholder(&core->video, core->frame_count, label);

   video->pixels = core->video.pixels;
   video->width = DEEVEE_VIDEO_WIDTH;
   video->height = DEEVEE_VIDEO_HEIGHT;
   video->pitch = core->video.pitch;
   audio->samples = deevee_audio_silence(&core->audio, &audio->frames);

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
