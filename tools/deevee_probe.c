#include "deevee_content.h"
#include "deevee_audio.h"
#include "deevee_decoder.h"
#include "deevee_disc.h"
#include "deevee_dvd.h"
#include "deevee_dvdnav.h"
#include "deevee_iso.h"
#include "deevee_subpicture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef HAVE_CHD
#define HAVE_CHD 0
#endif

#if HAVE_CHD
#include <libchdr/chd.h>
#endif

#ifdef _WIN32
#define probe_stat_type struct _stati64
#define probe_stat_call _stati64
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFREG) != 0)
#endif
#else
#define probe_stat_type struct stat
#define probe_stat_call stat
#endif

static int probe_stat_path(const char *path, probe_stat_type *st)
{
   return path && path[0] && probe_stat_call(path, st) == 0;
}

static const char *yes_no(int value)
{
   return value ? "yes" : "no";
}

static uint16_t probe_read_be16(const uint8_t *data)
{
   return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int sector_has_iso_volume_descriptor(const uint8_t *sector)
{
   return sector &&
      sector[0] >= 1 && sector[0] <= 3 &&
      memcmp(sector + 1, "CD001", 5) == 0;
}

struct decode_probe_context
{
   struct deevee_video_decoder *decoder;
   struct deevee_decoder_frame_probe *probe;
   enum deevee_decoder_status status;
   uint32_t payload_count;
   uint32_t payload_bytes;
};

struct packet_probe_context
{
   uint32_t packet_count;
   uint32_t video_packet_count;
   uint32_t packet_with_pts_count;
   uint32_t packet_with_dts_count;
   uint32_t printed_packets;
   bool last_pts_valid;
   uint64_t last_pts;
   bool pts_monotonic;
};

struct spu_decode_probe_context
{
   struct deevee_subpicture_decoder decoder;
   struct deevee_subpicture_frame frame;
   uint8_t *assembly;
   size_t assembly_size;
   size_t assembly_capacity;
   size_t expected_size;
   uint8_t stream_id;
   bool has_stream_id;
   uint32_t payload_count;
   uint32_t unit_count;
   uint32_t decoded_units;
   uint32_t rect_count;
   uint32_t decode_errors;
};

struct timed_decode_probe_context
{
   struct deevee_video_decoder *decoder;
   struct deevee_decoder_frame_probe *probe;
   enum deevee_decoder_status status;
   uint32_t payload_count;
   uint32_t payload_bytes;
   uint32_t printed_frame_pts;
   uint32_t decode_error_count;
};

struct audio_probe_context
{
   struct deevee_audio audio;
   enum deevee_audio_status status;
   uint32_t ac3_payloads;
   uint32_t ac3_payload_bytes;
   uint32_t skipped_packets;
};

static bool decode_payload_callback(const uint8_t *payload,
      size_t payload_size, void *user_data)
{
   struct decode_probe_context *context =
      (struct decode_probe_context *)user_data;

   context->status = deevee_decoder_decode_mpeg2_payload(context->decoder,
         payload, payload_size, context->probe);
   context->payload_count++;
   context->payload_bytes += (uint32_t)payload_size;
   return context->status == DEEVEE_DECODER_OK;
}

static bool title_packet_probe_callback(
      const struct deevee_dvd_packet *packet, void *user_data)
{
   struct packet_probe_context *context =
      (struct packet_probe_context *)user_data;

   if (!packet || !context)
      return false;

   context->packet_count++;
   if (packet->stream_id >= 0xe0 && packet->stream_id <= 0xef)
   {
      context->video_packet_count++;
      if (packet->has_pts)
      {
         context->packet_with_pts_count++;
         if (context->last_pts_valid && packet->pts < context->last_pts)
            context->pts_monotonic = false;
         context->last_pts = packet->pts;
         context->last_pts_valid = true;
      }
      if (packet->has_dts)
         context->packet_with_dts_count++;

      if (context->printed_packets < 8u)
      {
         printf("  resolved_title_packet_%u: stream=0x%02x vob=%u "
               "sector=%u bytes=%u pts=%s%llu dts=%s%llu scr=%s%llu\n",
               context->printed_packets + 1u, packet->stream_id,
               packet->vob_index, packet->logical_sector,
               (unsigned)packet->payload_size,
               packet->has_pts ? "" : "none:",
               (unsigned long long)(packet->has_pts ? packet->pts : 0),
               packet->has_dts ? "" : "none:",
               (unsigned long long)(packet->has_dts ? packet->dts : 0),
               packet->has_scr ? "" : "none:",
               (unsigned long long)(packet->has_scr ? packet->scr : 0));
         context->printed_packets++;
      }
   }

   return context->video_packet_count < 256u;
}

static bool timed_decode_packet_callback(
      const struct deevee_dvd_packet *packet, void *user_data)
{
   struct timed_decode_probe_context *context =
      (struct timed_decode_probe_context *)user_data;
   uint32_t frames_before;

   if (!packet || !context)
      return false;
   if (packet->stream_id < 0xe0 || packet->stream_id > 0xef)
      return true;

   frames_before = context->probe->frames_decoded;
   context->status = deevee_decoder_decode_mpeg2_timed_payload(
         context->decoder, packet->payload, packet->payload_size,
         packet->has_pts, packet->pts, packet->has_dts, packet->dts,
         context->probe);
   context->payload_count++;
   context->payload_bytes += (uint32_t)packet->payload_size;
   if (context->status != DEEVEE_DECODER_OK)
   {
      context->decode_error_count++;
      context->status = DEEVEE_DECODER_OK;
      return context->payload_count < 512u;
   }

   if (context->probe->frames_decoded > frames_before &&
         context->probe->has_pts && context->printed_frame_pts < 8u)
   {
      printf("  resolved_title_frame_%u_pts: %lld\n",
            context->printed_frame_pts + 1u,
            (long long)context->probe->pts);
      context->printed_frame_pts++;
   }

   return context->status == DEEVEE_DECODER_OK &&
      context->probe->frames_decoded < 16u;
}

static bool audio_probe_packet_callback(
      const struct deevee_dvd_packet *packet, void *user_data)
{
   struct audio_probe_context *context =
      (struct audio_probe_context *)user_data;

   if (!packet || !context)
      return false;

   if (packet->stream_id != 0xbd || !packet->payload ||
         packet->payload_size <= 4u)
      return true;

   if (packet->payload[0] < 0x80 || packet->payload[0] > 0x87)
   {
      context->skipped_packets++;
      return true;
   }

   context->status = deevee_audio_decode_ac3_payload(&context->audio,
         packet->payload + 4u, packet->payload_size - 4u);
   context->ac3_payloads++;
   context->ac3_payload_bytes += (uint32_t)(packet->payload_size - 4u);
   if (context->status != DEEVEE_AUDIO_OK)
   {
      context->audio.decode_errors++;
      return context->ac3_payloads < 128u;
   }

   return deevee_audio_decoded_frames(&context->audio) <
      DEEVEE_AUDIO_SAMPLE_RATE;
}

static bool spu_decode_probe_decode_unit(struct spu_decode_probe_context *context)
{
   enum deevee_subpicture_status status;

   if (!context || !context->assembly || !context->expected_size)
      return false;

   status = deevee_subpicture_decode_dvd_payload(&context->decoder,
         context->assembly, context->expected_size, false, 0,
         &context->frame);
   context->unit_count++;
   if (status != DEEVEE_SUBPICTURE_OK)
   {
      context->decode_errors++;
      return true;
   }
   if (context->frame.valid && context->frame.rect_count)
   {
      context->decoded_units++;
      context->rect_count += context->frame.rect_count;
   }
   return true;
}

static bool spu_decode_probe_packet_callback(
      const struct deevee_dvd_packet *packet, void *user_data)
{
   struct spu_decode_probe_context *context =
      (struct spu_decode_probe_context *)user_data;
   const uint8_t *payload;
   size_t payload_size;
   size_t required_size;

   if (!packet || !context)
      return false;
   if (packet->stream_id != 0xbd || !packet->payload ||
         packet->payload_size <= 1u)
      return true;

   if (packet->payload[0] < 0x20 || packet->payload[0] > 0x3f)
      return true;
   if (context->has_stream_id && packet->payload[0] != context->stream_id)
      return true;
   if (!context->has_stream_id)
   {
      context->stream_id = packet->payload[0];
      context->has_stream_id = true;
   }

   payload = packet->payload + 1u;
   payload_size = packet->payload_size - 1u;
   required_size = context->assembly_size + payload_size;
   if (required_size > context->assembly_capacity)
   {
      size_t new_capacity = context->assembly_capacity ?
         context->assembly_capacity * 2u : 4096u;
      uint8_t *new_assembly;

      while (new_capacity < required_size)
         new_capacity *= 2u;
      new_assembly = (uint8_t *)realloc(context->assembly, new_capacity);
      if (!new_assembly)
         return false;

      context->assembly = new_assembly;
      context->assembly_capacity = new_capacity;
   }

   memcpy(context->assembly + context->assembly_size, payload, payload_size);
   context->assembly_size += payload_size;
   context->payload_count++;

   if (!context->expected_size && context->assembly_size >= 2u)
      context->expected_size = probe_read_be16(context->assembly);

   while (context->expected_size &&
         context->assembly_size >= context->expected_size)
   {
      size_t unit_size = context->expected_size;
      size_t remaining;

      if (!spu_decode_probe_decode_unit(context))
         return false;

      remaining = context->assembly_size - unit_size;
      if (remaining)
         memmove(context->assembly, context->assembly + unit_size,
               remaining);
      context->assembly_size = remaining;
      context->expected_size = 0;
      if (context->assembly_size >= 2u)
         context->expected_size = probe_read_be16(context->assembly);
   }

   return context->decoded_units < 4u && context->payload_count < 256u;
}

static int dvdnav_block_has_start_code(const uint8_t *block, uint8_t code)
{
   size_t i;

   if (!block)
      return 0;

   for (i = 0; i + 3u < DEEVEE_DVDNAV_BLOCK_SIZE; i++)
   {
      if (block[i] == 0x00 && block[i + 1u] == 0x00 &&
            block[i + 2u] == 0x01 && block[i + 3u] == code)
         return 1;
   }

   return 0;
}

static void read_dvdnav_probe_window(struct deevee_dvdnav *nav,
      const char *prefix, unsigned max_events, unsigned *blocks,
      unsigned *nav_packets, unsigned *video_blocks, unsigned *highlights,
      int stop_on_nav)
{
   unsigned i;

   for (i = 0; i < max_events; i++)
   {
      struct deevee_dvdnav_event event;
      enum deevee_dvdnav_status status = deevee_dvdnav_next(nav, &event);
      const char *name = deevee_dvdnav_event_name(event.event);

      if (status != DEEVEE_DVDNAV_OK)
      {
         printf("  dvdnav_%s_read_status: %s\n", prefix,
               deevee_dvdnav_status_name(status));
         if (deevee_dvdnav_error(nav)[0])
            printf("  dvdnav_%s_error: %s\n", prefix,
                  deevee_dvdnav_error(nav));
         return;
      }

      if (i < 24u)
         printf("  dvdnav_%s_event_%u: %s len=%d\n", prefix, i + 1u, name,
               event.length);

      if (strcmp(name, "block") == 0)
      {
         (*blocks)++;
         if (dvdnav_block_has_start_code(event.data, 0xe0))
            (*video_blocks)++;
      }
      else if (strcmp(name, "nav") == 0)
      {
         (*nav_packets)++;
         if (stop_on_nav)
            return;
      }
      else if (strcmp(name, "highlight") == 0)
         (*highlights)++;
      else if (strcmp(name, "stop") == 0)
         return;

      deevee_dvdnav_ack_event(nav, event.event);
   }
}

static int find_special_features_command(const struct deevee_content_info *info,
      uint8_t command[8], int *button)
{
   struct deevee_disc disc;
   struct deevee_dvd_info dvd_info;
   struct deevee_dvd_title_table title_table;
   struct deevee_dvd_menu_render_probe render_probe;
   enum deevee_disc_status disc_status;
   enum deevee_dvd_status dvd_status;
   size_t i;
   int found = 0;

   if (!info || !command || !button)
      return 0;

   deevee_disc_init(&disc);
   disc_status = deevee_disc_open(&disc, info);
   if (disc_status != DEEVEE_DISC_OK)
      return 0;

   memset(&dvd_info, 0, sizeof(dvd_info));
   memset(&title_table, 0, sizeof(title_table));
   memset(&render_probe, 0, sizeof(render_probe));

   dvd_status = deevee_dvd_probe(&disc, &dvd_info);
   if (dvd_status == DEEVEE_DVD_OK)
      dvd_status = deevee_dvd_read_title_table(&disc, &dvd_info,
            &title_table);
   if (dvd_status == DEEVEE_DVD_OK)
      dvd_status = deevee_dvd_probe_vts_menu_pgc_render_streams(&disc, 1, 0,
            &render_probe);

   if (dvd_status == DEEVEE_DVD_OK)
   {
      for (i = 0; i < render_probe.button_count; i++)
      {
         struct deevee_dvd_playback_target target;
         const struct deevee_dvd_menu_button *candidate =
            &render_probe.buttons[i];

         memset(&target, 0, sizeof(target));
         if (!deevee_dvd_decode_playback_target_command(candidate->command, 1,
                  DEEVEE_DVD_MENU_DOMAIN_VTS, &title_table, &target))
            continue;

         if (target.type == DEEVEE_DVD_PLAYBACK_TARGET_MENU &&
               target.menu_domain == DEEVEE_DVD_MENU_DOMAIN_VMG &&
               target.pgc_number == 2)
         {
            memcpy(command, candidate->command, 8u);
            *button = candidate->number ? candidate->number : (int)(i + 1u);
            found = 1;
            break;
         }
      }
   }

   deevee_disc_close(&disc);
   return found;
}

static void print_dvdnav_probe(const struct deevee_content_info *info)
{
   struct deevee_dvdnav nav;
   enum deevee_dvdnav_status status;
   unsigned pre_blocks = 0;
   unsigned pre_nav_packets = 0;
   unsigned pre_video_blocks = 0;
   unsigned pre_highlights = 0;
   unsigned post_blocks = 0;
   unsigned post_nav_packets = 0;
   unsigned post_video_blocks = 0;
   unsigned post_highlights = 0;
   uint8_t special_command[8];
   int special_button = 0;
   int command_found;
   int select_ok;
   int activate_ok;

   printf("  dvdnav_available: %s\n", yes_no(deevee_dvdnav_available()));
   if (!deevee_dvdnav_available())
      return;

   deevee_dvdnav_init(&nav);
   status = deevee_dvdnav_open(&nav, info);
   printf("  dvdnav_open_status: %s\n", deevee_dvdnav_status_name(status));
   if (deevee_dvdnav_error(&nav)[0])
      printf("  dvdnav_open_message: %s\n", deevee_dvdnav_error(&nav));
   if (status != DEEVEE_DVDNAV_OK)
   {
      deevee_dvdnav_close(&nav);
      return;
   }

   printf("  dvdnav_root_menu_call: %s\n",
         yes_no(deevee_dvdnav_menu_call_root(&nav)));
   read_dvdnav_probe_window(&nav, "root", 64u, &pre_blocks,
         &pre_nav_packets, &pre_video_blocks, &pre_highlights, 1);

   select_ok = deevee_dvdnav_select_button(&nav, 2);
   if (!select_ok)
      select_ok = deevee_dvdnav_button(&nav, DEEVEE_DVDNAV_BUTTON_DOWN);
   printf("  dvdnav_special_features_select: %s\n", yes_no(select_ok));
   activate_ok = deevee_dvdnav_button(&nav, DEEVEE_DVDNAV_BUTTON_ACTIVATE);
   printf("  dvdnav_special_features_activate: %s\n", yes_no(activate_ok));
   memset(special_command, 0, sizeof(special_command));
   command_found = find_special_features_command(info, special_command,
         &special_button);
   printf("  dvdnav_special_features_command_found: %s\n",
         yes_no(command_found));
   printf("  dvdnav_special_features_command_button: %d\n",
         command_found ? special_button : 0);

   read_dvdnav_probe_window(&nav, "special_features", 768u, &post_blocks,
         &post_nav_packets, &post_video_blocks, &post_highlights, 0);

   printf("  dvdnav_root_blocks: %u\n", pre_blocks);
   printf("  dvdnav_root_nav_packets: %u\n", pre_nav_packets);
   printf("  dvdnav_root_video_blocks: %u\n", pre_video_blocks);
   printf("  dvdnav_root_highlights: %u\n", pre_highlights);
   printf("  dvdnav_special_features_blocks: %u\n", post_blocks);
   printf("  dvdnav_special_features_nav_packets: %u\n", post_nav_packets);
   printf("  dvdnav_special_features_video_blocks: %u\n", post_video_blocks);
   printf("  dvdnav_special_features_highlights: %u\n", post_highlights);

   deevee_dvdnav_close(&nav);
}

static void print_menu_vob_decode_probe(struct deevee_disc *disc,
      const char *iso_path, unsigned index)
{
   struct deevee_video_decoder decoder;
   struct deevee_decoder_frame_probe frame_probe;
   struct decode_probe_context decode_context;
   enum deevee_decoder_status decoder_status;
   enum deevee_dvd_status dvd_status;

   memset(&frame_probe, 0, sizeof(frame_probe));
   memset(&decode_context, 0, sizeof(decode_context));

   decoder_status = deevee_decoder_init(&decoder);
   if (decoder_status == DEEVEE_DECODER_OK)
      decoder_status = deevee_decoder_open_mpeg2(&decoder);

   if (decoder_status == DEEVEE_DECODER_OK)
   {
      decode_context.decoder = &decoder;
      decode_context.probe = &frame_probe;
      decode_context.status = DEEVEE_DECODER_OK;

      dvd_status = deevee_dvd_walk_vob_video_payloads(disc, iso_path,
            decode_payload_callback, &decode_context);
      if (dvd_status != DEEVEE_DVD_OK)
         decoder_status = DEEVEE_DECODER_ERROR_DECODE_FAILED;
      else if (decode_context.status != DEEVEE_DECODER_OK)
         decoder_status = decode_context.status;
      else
         decoder_status = deevee_decoder_flush_mpeg2(&decoder, &frame_probe);
   }

   printf("  menu_vob_candidate_%u_path: %s\n", index, iso_path);
   printf("  menu_vob_candidate_%u_decode_status: %s\n", index,
         deevee_decoder_status_name(decoder_status));
   printf("  menu_vob_candidate_%u_payloads: %u\n", index,
         decode_context.payload_count);
   printf("  menu_vob_candidate_%u_payload_bytes: %u\n", index,
         decode_context.payload_bytes);
   printf("  menu_vob_candidate_%u_packets_sent: %u\n", index,
         frame_probe.packets_sent);
   printf("  menu_vob_candidate_%u_frames: %u\n", index,
         frame_probe.frames_decoded);
   if (frame_probe.got_frame)
   {
      printf("  menu_vob_candidate_%u_width: %u\n", index,
            frame_probe.width);
      printf("  menu_vob_candidate_%u_height: %u\n", index,
            frame_probe.height);
      printf("  menu_vob_candidate_%u_pixel_format: %d\n", index,
            frame_probe.pixel_format);
   }

   deevee_decoder_deinit(&decoder);
}

static void print_menu_vob_render_probe(struct deevee_disc *disc,
      const struct deevee_dvd_title_table *title_table,
      const char *iso_path, unsigned index)
{
   struct deevee_dvd_menu_render_probe render_probe;
   enum deevee_dvd_status status;
   uint8_t button_index;

   memset(&render_probe, 0, sizeof(render_probe));
   status = deevee_dvd_probe_vob_menu_render_streams(disc, iso_path,
         &render_probe);
   printf("  menu_vob_candidate_%u_render_status: %s\n", index,
         deevee_dvd_status_name(status));
   if (status != DEEVEE_DVD_OK)
      return;

   printf("  menu_vob_candidate_%u_render_buttons: %u\n", index,
         render_probe.button_count);
   printf("  menu_vob_candidate_%u_render_has_nav: %s\n", index,
         yes_no(render_probe.has_nav));
   printf("  menu_vob_candidate_%u_render_has_subpicture: %s\n", index,
         yes_no(render_probe.has_subpicture));

   for (button_index = 0; button_index < render_probe.button_count;
         button_index++)
   {
      const struct deevee_dvd_menu_button *button =
         &render_probe.buttons[button_index];
      struct deevee_dvd_playback_target target;
      bool decoded;

      memset(&target, 0, sizeof(target));
      decoded = deevee_dvd_decode_playback_target_command(button->command,
            0, DEEVEE_DVD_MENU_DOMAIN_NONE, title_table, &target);
      printf("  menu_vob_candidate_%u_render_button_%u_command: "
            "%02x%02x%02x%02x%02x%02x%02x%02x\n", index,
            (unsigned)(button_index + 1u), button->command[0],
            button->command[1], button->command[2], button->command[3],
            button->command[4], button->command[5], button->command[6],
            button->command[7]);
      printf("  menu_vob_candidate_%u_render_button_%u_decoded: %s\n",
            index, (unsigned)(button_index + 1u), yes_no(decoded));
      if (decoded)
      {
         printf("  menu_vob_candidate_%u_render_button_%u_target_type: %s\n",
               index, (unsigned)(button_index + 1u),
               deevee_dvd_playback_target_type_name(target.type));
         printf("  menu_vob_candidate_%u_render_button_%u_target_vts: %u\n",
               index, (unsigned)(button_index + 1u), target.vts_number);
         printf("  menu_vob_candidate_%u_render_button_%u_target_vts_title: %u\n",
               index, (unsigned)(button_index + 1u),
               target.vts_title_number);
      }
   }
}

static void print_menu_vob_spu_decode_probe(struct deevee_disc *disc,
      const char *iso_path, unsigned index)
{
   struct spu_decode_probe_context context;
   enum deevee_dvd_status status;

   memset(&context, 0, sizeof(context));
   deevee_subpicture_init(&context.decoder);
   if (deevee_subpicture_open_dvd(&context.decoder) !=
         DEEVEE_SUBPICTURE_OK)
   {
      printf("  menu_vob_candidate_%u_spu_decoder: unavailable\n", index);
      return;
   }

   status = deevee_dvd_walk_vob_packets(disc, iso_path,
         spu_decode_probe_packet_callback, &context);

   printf("  menu_vob_candidate_%u_spu_probe_status: %s\n", index,
         deevee_dvd_status_name(status));
   printf("  menu_vob_candidate_%u_spu_stream: 0x%02x\n", index,
         context.has_stream_id ? context.stream_id : 0);
   printf("  menu_vob_candidate_%u_spu_payloads: %u\n", index,
         context.payload_count);
   printf("  menu_vob_candidate_%u_spu_units: %u\n", index,
         context.unit_count);
   printf("  menu_vob_candidate_%u_spu_decoded_units: %u\n", index,
         context.decoded_units);
   printf("  menu_vob_candidate_%u_spu_rects: %u\n", index,
         context.rect_count);
   printf("  menu_vob_candidate_%u_spu_errors: %u\n", index,
         context.decode_errors);

   free(context.assembly);
   deevee_subpicture_frame_clear(&context.frame);
   deevee_subpicture_deinit(&context.decoder);
}

static int command_is_link_tail_pgc(const uint8_t command[8])
{
   return command && command[0] == 0x20 && (command[1] & 0x0f) == 0x01 &&
      (command[7] & 0x1f) == 0x0d;
}

static int command_is_direct_jump(const uint8_t command[8])
{
   return command && command[0] == 0x30 &&
      ((command[1] & 0x0f) == 0x02 ||
       (command[1] & 0x0f) == 0x03 ||
       (command[1] & 0x0f) == 0x05 ||
       (command[1] & 0x0f) == 0x06);
}

static const uint8_t *resolve_probe_button_command(
      const struct deevee_dvd_menu_render_probe *render_probe)
{
   uint8_t i;

   if (!render_probe || !render_probe->button_count)
      return NULL;

   if (command_is_direct_jump(render_probe->buttons[0].command))
      return render_probe->buttons[0].command;

   if (!command_is_link_tail_pgc(render_probe->buttons[0].command))
      return NULL;

   for (i = 0; i < render_probe->parsed_post_command_count; i++)
      if (command_is_direct_jump(render_probe->post_commands[i]))
         return render_probe->post_commands[i];

   return NULL;
}

static void print_title_decode_probe(struct deevee_disc *disc,
      const struct deevee_dvd_title_pgc *title_pgc)
{
   struct deevee_video_decoder decoder;
   struct deevee_decoder_frame_probe frame_probe;
   struct timed_decode_probe_context decode_context;
   enum deevee_decoder_status decoder_status;
   enum deevee_dvd_status dvd_status;

   memset(&frame_probe, 0, sizeof(frame_probe));
   memset(&decode_context, 0, sizeof(decode_context));

   decoder_status = deevee_decoder_init(&decoder);
   if (decoder_status == DEEVEE_DECODER_OK)
      decoder_status = deevee_decoder_open_mpeg2(&decoder);

   if (decoder_status == DEEVEE_DECODER_OK)
   {
      decode_context.decoder = &decoder;
      decode_context.probe = &frame_probe;
      decode_context.status = DEEVEE_DECODER_OK;

      dvd_status = deevee_dvd_walk_title_pgc_packets(disc, title_pgc,
            timed_decode_packet_callback, &decode_context);
      if (dvd_status != DEEVEE_DVD_OK)
         decoder_status = DEEVEE_DECODER_ERROR_DECODE_FAILED;
      else if (decode_context.status != DEEVEE_DECODER_OK)
         decoder_status = decode_context.status;
      else
         decoder_status = deevee_decoder_flush_mpeg2(&decoder, &frame_probe);
   }

   printf("  resolved_title_decode_status: %s\n",
         deevee_decoder_status_name(decoder_status));
   printf("  resolved_title_decode_payloads: %u\n",
         decode_context.payload_count);
   printf("  resolved_title_decode_payload_bytes: %u\n",
         decode_context.payload_bytes);
   printf("  resolved_title_decode_errors: %u\n",
         decode_context.decode_error_count);
   printf("  resolved_title_decode_packets_sent: %u\n",
         frame_probe.packets_sent);
   printf("  resolved_title_decode_frames: %u\n",
         frame_probe.frames_decoded);
   printf("  resolved_title_decode_got_frame: %s\n",
         yes_no(frame_probe.got_frame));
   if (frame_probe.got_frame)
   {
      printf("  resolved_title_decode_width: %u\n", frame_probe.width);
      printf("  resolved_title_decode_height: %u\n", frame_probe.height);
      printf("  resolved_title_decode_pixel_format: %d\n",
            frame_probe.pixel_format);
   }

   deevee_decoder_deinit(&decoder);
}

static void print_title_packet_probe(struct deevee_disc *disc,
      const struct deevee_dvd_title_pgc *title_pgc)
{
   struct packet_probe_context context;
   enum deevee_dvd_status status;

   memset(&context, 0, sizeof(context));
   context.pts_monotonic = true;
   status = deevee_dvd_walk_title_pgc_packets(disc, title_pgc,
         title_packet_probe_callback, &context);

   printf("  resolved_title_packet_probe_status: %s\n",
         deevee_dvd_status_name(status));
   printf("  resolved_title_packets_scanned: %u\n", context.packet_count);
   printf("  resolved_title_video_packets_scanned: %u\n",
         context.video_packet_count);
   printf("  resolved_title_video_packets_with_pts: %u\n",
         context.packet_with_pts_count);
   printf("  resolved_title_video_packets_with_dts: %u\n",
         context.packet_with_dts_count);
   printf("  resolved_title_video_pts_monotonic: %s\n",
         yes_no(context.pts_monotonic));
}

static void print_title_audio_probe(struct deevee_disc *disc,
      const struct deevee_dvd_title_pgc *title_pgc)
{
   struct audio_probe_context context;
   enum deevee_dvd_status status;

   memset(&context, 0, sizeof(context));
   deevee_audio_init(&context.audio);
   context.status = DEEVEE_AUDIO_OK;

   status = deevee_dvd_walk_title_pgc_packets(disc, title_pgc,
         audio_probe_packet_callback, &context);

   printf("  resolved_title_audio_probe_status: %s\n",
         deevee_dvd_status_name(status));
   printf("  resolved_title_audio_decode_status: %d\n",
         (int)context.status);
   printf("  resolved_title_audio_ac3_payloads: %u\n",
         context.ac3_payloads);
   printf("  resolved_title_audio_ac3_payload_bytes: %u\n",
         context.ac3_payload_bytes);
   printf("  resolved_title_audio_packets_sent: %llu\n",
         (unsigned long long)deevee_audio_packets_sent(&context.audio));
   printf("  resolved_title_audio_frames: %llu\n",
         (unsigned long long)deevee_audio_decoded_frames(&context.audio));
   printf("  resolved_title_audio_buffered_frames: %u\n",
         (unsigned)deevee_audio_buffered_frames(&context.audio));
   printf("  resolved_title_audio_decode_errors: %llu\n",
         (unsigned long long)deevee_audio_decode_errors(&context.audio));
   printf("  resolved_title_audio_sample_rate: %u\n",
         deevee_audio_last_sample_rate(&context.audio));
   printf("  resolved_title_audio_channels: %u\n",
         deevee_audio_last_channels(&context.audio));
   printf("  resolved_title_audio_skipped_packets: %u\n",
         context.skipped_packets);

   deevee_audio_deinit(&context.audio);
}

static void print_disc_probe(const struct deevee_content_info *info)
{
   struct deevee_disc disc;
   enum deevee_disc_status status;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];

   deevee_disc_init(&disc);
   status = deevee_disc_open(&disc, info);
   printf("  reader_status: %s\n", deevee_disc_status_name(status));

#if HAVE_CHD
   if (info && info->type == DEEVEE_CONTENT_CHD)
   {
      chd_header header;
      chd_error chd_status = chd_read_header(info->path, &header);

      printf("  chd_header_status: %s\n", chd_error_string(chd_status));
      if (chd_status == CHDERR_NONE)
      {
         chd_file *chd = NULL;
         chd_error open_status;

         printf("  chd_version: %u\n", header.version);
         printf("  chd_hunk_bytes: %u\n", header.hunkbytes);
         printf("  chd_total_hunks: %u\n", header.totalhunks);
         printf("  chd_logical_bytes: %llu\n",
               (unsigned long long)header.logicalbytes);
         printf("  chd_unit_bytes: %u\n", header.unitbytes);
         printf("  chd_unit_count: %llu\n",
               (unsigned long long)header.unitcount);

         open_status = chd_open(info->path, CHD_OPEN_READ, NULL, &chd);
         if (open_status == CHDERR_NONE && header.unitbytes &&
               header.hunkbytes >= header.unitbytes)
         {
            uint32_t units_per_hunk = header.hunkbytes / header.unitbytes;
            uint32_t hunk = 16u / units_per_hunk;
            uint32_t unit = 16u % units_per_hunk;
            uint8_t *hunk_data = (uint8_t *)malloc(header.hunkbytes);

            if (hunk_data && chd_read(chd, hunk, hunk_data) == CHDERR_NONE)
            {
               uint32_t offset;
               int found = 0;
               const uint8_t *unit_data = hunk_data +
                  (size_t)unit * header.unitbytes;

               for (offset = 0; offset + 5u <= header.unitbytes; offset++)
               {
                  if (memcmp(unit_data + offset, "CD001", 5) == 0)
                  {
                     printf("  chd_sector16_cd001_offset: %u\n", offset);
                     found = 1;
                  }
               }

               if (!found)
                  printf("  chd_sector16_cd001_offset: missing\n");
            }

            free(hunk_data);
         }

         if (chd)
            chd_close(chd);
      }
   }
#endif

   if (status == DEEVEE_DISC_OK)
   {
      struct deevee_iso_entry video_ts_ifo;
      enum deevee_iso_status iso_status;
      struct deevee_dvd_info dvd_info;
      enum deevee_dvd_status dvd_status;

      printf("  sector_size: %u\n", DEEVEE_DVD_SECTOR_SIZE);
      printf("  sector_count: %llu\n",
            (unsigned long long)disc.sector_count);

      status = deevee_disc_read_sector(&disc, 16, sector, sizeof(sector));
      printf("  volume_descriptor_status: %s\n",
            deevee_disc_status_name(status));
      if (status == DEEVEE_DISC_OK)
         printf("  volume_descriptor: %s\n",
               sector_has_iso_volume_descriptor(sector) ? "present" : "missing");

      iso_status = deevee_iso_find_path(&disc,
            "/VIDEO_TS/VIDEO_TS.IFO", &video_ts_ifo);
      printf("  video_ts_ifo_status: %s\n",
            deevee_iso_status_name(iso_status));
      if (iso_status == DEEVEE_ISO_OK)
      {
         printf("  video_ts_ifo_lba: %u\n", video_ts_ifo.lba);
         printf("  video_ts_ifo_size: %u\n", video_ts_ifo.size);
      }

      dvd_status = deevee_dvd_probe(&disc, &dvd_info);
      printf("  dvd_probe_status: %s\n",
            deevee_dvd_status_name(dvd_status));
      if (dvd_status == DEEVEE_DVD_OK ||
            dvd_status == DEEVEE_DVD_ERROR_INVALID_VMG)
      {
         struct deevee_dvd_title_table title_table;
         struct deevee_dvd_menu_language_table menu_table;
         struct deevee_dvd_menu_pgc_summary menu_pgc;
         struct deevee_dvd_menu_pgc_table_summary menu_pgc_tables;
         struct deevee_dvd_menu_vob_span menu_vob_span;
         struct deevee_dvd_vob_packet_probe vob_packet_probe;
         struct deevee_dvd_video_probe video_probe;
         enum deevee_dvd_status title_status;
         enum deevee_dvd_status menu_status;
         enum deevee_dvd_status menu_pgc_status;
         enum deevee_dvd_status menu_pgc_tables_status;
         enum deevee_dvd_status menu_vob_span_status;
         enum deevee_dvd_status vob_packet_probe_status;
         enum deevee_dvd_status video_probe_status;
         enum deevee_decoder_status decoder_status;
         uint16_t i;

         printf("  dvd_video: %s\n", yes_no(dvd_info.is_dvd_video));
         printf("  video_ts_ifo_identifier: %s\n",
               dvd_info.vmg_identifier[0] ? dvd_info.vmg_identifier : "(empty)");
         printf("  vmg_last_sector: %u\n", dvd_info.vmg_last_sector);
         printf("  vmgi_last_sector: %u\n", dvd_info.vmgi_last_sector);
         printf("  vmg_title_set_count: %u\n", dvd_info.vmg_title_set_count);
         printf("  vmgi_last_byte: %u\n", dvd_info.vmgi_last_byte);
         printf("  first_play_pgc_sector: %u\n", dvd_info.first_play_pgc);
         printf("  vmgm_vobs_sector: %u\n", dvd_info.vmgm_vobs);
         printf("  tt_srpt_sector: %u\n", dvd_info.tt_srpt);
         printf("  vmgm_pgci_ut_sector: %u\n", dvd_info.vmgm_pgci_ut);
         printf("  ptl_mait_sector: %u\n", dvd_info.ptl_mait);
         printf("  vts_atrt_sector: %u\n", dvd_info.vts_atrt);
         printf("  txtdt_mgi_sector: %u\n", dvd_info.txtdt_mgi);
         printf("  vmgm_c_adt_sector: %u\n", dvd_info.vmgm_c_adt);
         printf("  vmgm_vobu_admap_sector: %u\n", dvd_info.vmgm_vobu_admap);

         title_status = deevee_dvd_read_title_table(&disc,
               &dvd_info, &title_table);
         printf("  title_table_status: %s\n",
               deevee_dvd_status_name(title_status));
         if (title_status == DEEVEE_DVD_OK)
         {
            printf("  title_count: %u\n", title_table.title_count);
            printf("  parsed_title_count: %u\n",
                  title_table.parsed_title_count);
            for (i = 0; i < title_table.parsed_title_count; i++)
            {
               const struct deevee_dvd_title *title =
                  &title_table.titles[i];
               printf("  title_%u_type: %u\n", i + 1, title->title_type);
               printf("  title_%u_angles: %u\n", i + 1, title->angle_count);
               printf("  title_%u_chapters: %u\n", i + 1,
                     title->chapter_count);
               printf("  title_%u_vts: %u\n", i + 1, title->vts_number);
               printf("  title_%u_vts_title: %u\n", i + 1,
                     title->vts_title_number);
               printf("  title_%u_vts_start_sector: %u\n", i + 1,
                     title->vts_start_sector);
            }
         }

         menu_status = deevee_dvd_read_menu_language_table(&disc,
               &dvd_info, &menu_table);
         printf("  menu_language_table_status: %s\n",
               deevee_dvd_status_name(menu_status));
         if (menu_status == DEEVEE_DVD_OK)
         {
            printf("  menu_language_count: %u\n",
                  menu_table.language_count);
            printf("  parsed_menu_language_count: %u\n",
                  menu_table.parsed_language_count);
            for (i = 0; i < menu_table.parsed_language_count; i++)
            {
               const struct deevee_dvd_menu_language_unit *language =
                  &menu_table.languages[i];
               printf("  menu_language_%u_code: %s\n", i + 1,
                     language->language);
               printf("  menu_language_%u_extension: %u\n", i + 1,
                     language->language_extension);
               printf("  menu_language_%u_existence: %u\n", i + 1,
                     language->menu_existence);
               printf("  menu_language_%u_start_byte: %u\n", i + 1,
                     language->start_byte);
            }
         }

         menu_pgc_status = deevee_dvd_read_first_menu_pgc(&disc,
               &dvd_info, &menu_pgc);
         printf("  first_menu_pgc_status: %s\n",
               deevee_dvd_status_name(menu_pgc_status));
         if (menu_pgc_status == DEEVEE_DVD_OK)
         {
            printf("  first_menu_pgc_language: %s\n", menu_pgc.language);
            printf("  first_menu_pgc_count: %u\n", menu_pgc.pgc_count);
            printf("  first_menu_pgc_category: %u\n",
                  menu_pgc.pgc_category);
            printf("  first_menu_pgc_start_byte: %u\n",
                  menu_pgc.pgc_start_byte);
            printf("  first_menu_program_count: %u\n",
                  menu_pgc.program_count);
            printf("  first_menu_cell_count: %u\n",
                  menu_pgc.cell_count);
            printf("  first_menu_playback_time_bcd: %02x:%02x:%02x:%02x\n",
                  menu_pgc.playback_time[0], menu_pgc.playback_time[1],
                  menu_pgc.playback_time[2], menu_pgc.playback_time[3]);
            printf("  first_menu_prohibited_user_ops: %u\n",
                  menu_pgc.prohibited_user_ops);
            printf("  first_menu_command_table_offset: %u\n",
                  menu_pgc.command_table_offset);
            printf("  first_menu_program_map_offset: %u\n",
                  menu_pgc.program_map_offset);
            printf("  first_menu_cell_playback_table_offset: %u\n",
                  menu_pgc.cell_playback_table_offset);
            printf("  first_menu_cell_position_table_offset: %u\n",
                  menu_pgc.cell_position_table_offset);
         }

         menu_pgc_tables_status = deevee_dvd_read_first_menu_pgc_tables(
               &disc, &dvd_info, &menu_pgc_tables);
         printf("  first_menu_pgc_tables_status: %s\n",
               deevee_dvd_status_name(menu_pgc_tables_status));
         if (menu_pgc_tables_status == DEEVEE_DVD_OK)
         {
            printf("  first_menu_pre_command_count: %u\n",
                  menu_pgc_tables.pre_command_count);
            printf("  first_menu_post_command_count: %u\n",
                  menu_pgc_tables.post_command_count);
            printf("  first_menu_cell_command_count: %u\n",
                  menu_pgc_tables.cell_command_count);
            printf("  first_menu_command_table_last_byte: %u\n",
                  menu_pgc_tables.command_table_last_byte);
            printf("  first_menu_first_program_entry_cell: %u\n",
                  menu_pgc_tables.first_program_entry_cell);
            printf("  first_menu_first_cell_category: %u\n",
                  menu_pgc_tables.first_cell_category);
            printf("  first_menu_first_cell_playback_time_bcd: "
                  "%02x:%02x:%02x:%02x\n",
                  menu_pgc_tables.first_cell_playback_time[0],
                  menu_pgc_tables.first_cell_playback_time[1],
                  menu_pgc_tables.first_cell_playback_time[2],
                  menu_pgc_tables.first_cell_playback_time[3]);
            printf("  first_menu_first_cell_first_vobu_start_sector: %u\n",
                  menu_pgc_tables.first_cell_first_vobu_start_sector);
            printf("  first_menu_first_cell_first_ilvu_end_sector: %u\n",
                  menu_pgc_tables.first_cell_first_ilvu_end_sector);
            printf("  first_menu_first_cell_last_vobu_start_sector: %u\n",
                  menu_pgc_tables.first_cell_last_vobu_start_sector);
            printf("  first_menu_first_cell_last_vobu_end_sector: %u\n",
                  menu_pgc_tables.first_cell_last_vobu_end_sector);
            printf("  first_menu_first_cell_vob_id: %u\n",
                  menu_pgc_tables.first_cell_vob_id);
            printf("  first_menu_first_cell_id: %u\n",
                  menu_pgc_tables.first_cell_id);
         }

         menu_vob_span_status = deevee_dvd_resolve_first_menu_vob_span(
               &disc, &dvd_info, &menu_vob_span);
         printf("  first_menu_vob_span_status: %s\n",
               deevee_dvd_status_name(menu_vob_span_status));
         if (menu_vob_span_status == DEEVEE_DVD_OK)
         {
            enum deevee_disc_status first_sector_status;

            printf("  vmgm_vob_lba: %u\n", menu_vob_span.vmgm_vob.lba);
            printf("  vmgm_vob_size: %u\n", menu_vob_span.vmgm_vob.size);
            printf("  vmgm_vob_sector_count: %u\n",
                  menu_vob_span.vmgm_vob_sector_count);
            printf("  first_menu_cell_relative_start_sector: %u\n",
                  menu_vob_span.first_cell_start_sector);
            printf("  first_menu_cell_relative_end_sector: %u\n",
                  menu_vob_span.first_cell_end_sector);
            printf("  first_menu_cell_start_lba: %llu\n",
                  (unsigned long long)menu_vob_span.first_cell_start_lba);
            printf("  first_menu_cell_end_lba: %llu\n",
                  (unsigned long long)menu_vob_span.first_cell_end_lba);

            first_sector_status = deevee_disc_read_sector(&disc,
                  menu_vob_span.first_cell_start_lba, sector, sizeof(sector));
            printf("  first_menu_cell_start_sector_read_status: %s\n",
                  deevee_disc_status_name(first_sector_status));
         }

         vob_packet_probe_status = deevee_dvd_probe_first_menu_vob_packets(
               &disc, &dvd_info, &vob_packet_probe);
         printf("  first_menu_vob_packet_probe_status: %s\n",
               deevee_dvd_status_name(vob_packet_probe_status));
         if (vob_packet_probe_status == DEEVEE_DVD_OK)
         {
            printf("  first_menu_vob_scanned_sectors: %u\n",
                  vob_packet_probe.scanned_sectors);
            printf("  first_menu_vob_pack_headers: %u\n",
                  vob_packet_probe.pack_header_count);
            printf("  first_menu_vob_system_headers: %u\n",
                  vob_packet_probe.system_header_count);
            printf("  first_menu_vob_program_stream_maps: %u\n",
                  vob_packet_probe.program_stream_map_count);
            printf("  first_menu_vob_private_stream_1_packets: %u\n",
                  vob_packet_probe.private_stream_1_count);
            printf("  first_menu_vob_private_stream_2_packets: %u\n",
                  vob_packet_probe.private_stream_2_count);
            printf("  first_menu_vob_padding_stream_packets: %u\n",
                  vob_packet_probe.padding_stream_count);
            printf("  first_menu_vob_video_pes_packets: %u\n",
                  vob_packet_probe.video_pes_count);
            printf("  first_menu_vob_audio_pes_packets: %u\n",
                  vob_packet_probe.audio_pes_count);
            printf("  first_menu_vob_ac3_audio_packets: %u\n",
                  vob_packet_probe.ac3_audio_count);
            printf("  first_menu_vob_dts_audio_packets: %u\n",
                  vob_packet_probe.dts_audio_count);
            printf("  first_menu_vob_lpcm_audio_packets: %u\n",
                  vob_packet_probe.lpcm_audio_count);
            printf("  first_menu_vob_subpicture_packets: %u\n",
                  vob_packet_probe.subpicture_count);
            printf("  first_menu_vob_private_stream_1_unknown_packets: %u\n",
                  vob_packet_probe.private_stream_1_unknown_count);
            printf("  first_menu_vob_nav_pci_packets: %u\n",
                  vob_packet_probe.nav_pci_count);
            printf("  first_menu_vob_nav_dsi_packets: %u\n",
                  vob_packet_probe.nav_dsi_count);
            printf("  first_menu_vob_private_stream_2_unknown_packets: %u\n",
                  vob_packet_probe.private_stream_2_unknown_count);
            printf("  first_menu_vob_other_pes_packets: %u\n",
                  vob_packet_probe.other_pes_count);
            printf("  first_menu_vob_nav_packets: %u\n",
                  vob_packet_probe.nav_pack_count);
            printf("  first_menu_vob_first_video_stream_id: 0x%02x\n",
                  vob_packet_probe.first_video_stream_id);
            printf("  first_menu_vob_first_audio_stream_id: 0x%02x\n",
                  vob_packet_probe.first_audio_stream_id);
            printf("  first_menu_vob_first_private_stream_1_substream_id: "
                  "0x%02x\n",
                  vob_packet_probe.first_private_stream_1_substream_id);
            printf("  first_menu_vob_first_nav_substream_id: 0x%02x\n",
                  vob_packet_probe.first_nav_substream_id);
            printf("  first_menu_vob_has_pack_header: %s\n",
                  yes_no(vob_packet_probe.has_pack_header));
            printf("  first_menu_vob_has_video: %s\n",
                  yes_no(vob_packet_probe.has_video));
            printf("  first_menu_vob_has_audio: %s\n",
                  yes_no(vob_packet_probe.has_audio));
            printf("  first_menu_vob_has_subpicture: %s\n",
                  yes_no(vob_packet_probe.has_subpicture));
            printf("  first_menu_vob_has_nav: %s\n",
                  yes_no(vob_packet_probe.has_nav));
         }

         video_probe_status = deevee_dvd_probe_first_menu_video(&disc,
               &dvd_info, &video_probe);
         printf("  first_menu_video_probe_status: %s\n",
               deevee_dvd_status_name(video_probe_status));
         if (video_probe_status == DEEVEE_DVD_OK)
         {
            printf("  first_menu_video_scanned_sectors: %u\n",
                  video_probe.scanned_sectors);
            printf("  first_menu_video_pes_packets: %u\n",
                  video_probe.video_pes_packets);
            printf("  first_menu_video_payload_bytes: %u\n",
                  video_probe.video_payload_bytes);
            printf("  first_menu_video_first_stream_id: 0x%02x\n",
                  video_probe.first_video_stream_id);
            printf("  first_menu_video_has_payload: %s\n",
                  yes_no(video_probe.has_video_payload));
            printf("  first_menu_video_has_sequence_header: %s\n",
                  yes_no(video_probe.has_sequence_header));
            if (video_probe.has_sequence_header)
            {
               printf("  first_menu_video_sequence_width: %u\n",
                     video_probe.sequence_width);
               printf("  first_menu_video_sequence_height: %u\n",
                     video_probe.sequence_height);
               printf("  first_menu_video_aspect_ratio_code: %u\n",
                     video_probe.aspect_ratio_code);
               printf("  first_menu_video_frame_rate_code: %u\n",
                     video_probe.frame_rate_code);
               printf("  first_menu_video_bit_rate_value: %u\n",
                     video_probe.bit_rate_value);
               printf("  first_menu_video_vbv_buffer_size_value: %u\n",
                     video_probe.vbv_buffer_size_value);
               printf("  first_menu_video_constrained_parameters: %s\n",
                     yes_no(video_probe.constrained_parameters_flag));
            }
         }

         {
            struct deevee_video_decoder decoder;
            struct deevee_decoder_frame_probe frame_probe;
            struct decode_probe_context decode_context;

            memset(&frame_probe, 0, sizeof(frame_probe));
            decoder_status = deevee_decoder_init(&decoder);
            if (decoder_status == DEEVEE_DECODER_OK)
               decoder_status = deevee_decoder_open_mpeg2(&decoder);

            if (decoder_status == DEEVEE_DECODER_OK)
            {
               decode_context.decoder = &decoder;
               decode_context.probe = &frame_probe;
               decode_context.status = DEEVEE_DECODER_OK;

               video_probe_status = deevee_dvd_walk_first_menu_video_payloads(
                     &disc, &dvd_info, decode_payload_callback,
                     &decode_context);
               if (video_probe_status != DEEVEE_DVD_OK)
                  decoder_status = DEEVEE_DECODER_ERROR_DECODE_FAILED;
               else if (decode_context.status != DEEVEE_DECODER_OK)
                  decoder_status = decode_context.status;
               else
                  decoder_status = deevee_decoder_flush_mpeg2(&decoder,
                        &frame_probe);
            }

            printf("  first_menu_decode_probe_status: %s\n",
                  deevee_decoder_status_name(decoder_status));
            printf("  first_menu_decode_packets_sent: %u\n",
                  frame_probe.packets_sent);
            printf("  first_menu_decode_frames: %u\n",
                  frame_probe.frames_decoded);
            printf("  first_menu_decode_got_frame: %s\n",
                  yes_no(frame_probe.got_frame));
            if (frame_probe.got_frame)
            {
               printf("  first_menu_decode_width: %u\n", frame_probe.width);
               printf("  first_menu_decode_height: %u\n", frame_probe.height);
               printf("  first_menu_decode_pixel_format: %d\n",
                     frame_probe.pixel_format);
            }

            deevee_decoder_deinit(&decoder);
         }

         {
            unsigned candidate_index = 1;
            unsigned vts;
            char vob_path[32];

            print_menu_vob_decode_probe(&disc, "/VIDEO_TS/VIDEO_TS.VOB",
                  candidate_index++);
            for (vts = 1; vts <= dvd_info.vmg_title_set_count && vts <= 99;
                  vts++)
            {
               snprintf(vob_path, sizeof(vob_path),
                     "/VIDEO_TS/VTS_%02u_0.VOB", vts);
               print_menu_vob_decode_probe(&disc, vob_path,
                     candidate_index++);
            }
         }

         {
            unsigned candidate_index = 1;
            unsigned vts;
            char vob_path[32];

            print_menu_vob_spu_decode_probe(&disc, "/VIDEO_TS/VIDEO_TS.VOB",
                  candidate_index++);
            for (vts = 1; vts <= dvd_info.vmg_title_set_count && vts <= 99;
                  vts++)
            {
               snprintf(vob_path, sizeof(vob_path),
                     "/VIDEO_TS/VTS_%02u_0.VOB", vts);
               print_menu_vob_spu_decode_probe(&disc, vob_path,
                     candidate_index++);
            }
         }

         {
            unsigned candidate_index = 1;
            unsigned vts;
            char vob_path[32];

            print_menu_vob_render_probe(&disc, &title_table,
                  "/VIDEO_TS/VIDEO_TS.VOB", candidate_index++);
            for (vts = 1; vts <= dvd_info.vmg_title_set_count && vts <= 99;
                  vts++)
            {
               snprintf(vob_path, sizeof(vob_path),
                     "/VIDEO_TS/VTS_%02u_0.VOB", vts);
               print_menu_vob_render_probe(&disc, &title_table, vob_path,
                     candidate_index++);
            }
         }

         {
            struct deevee_dvd_menu_render_probe render_probe;
            enum deevee_dvd_status render_status =
               deevee_dvd_probe_vts_menu_pgc_render_streams(&disc, 1, 0,
                     &render_probe);

            printf("  vts_menu_render_probe_status: %s\n",
                  deevee_dvd_status_name(render_status));
            if (render_status == DEEVEE_DVD_OK)
            {
               printf("  vts_menu_render_scanned_bytes: %u\n",
                     render_probe.scanned_bytes);
               printf("  vts_menu_render_video_packets: %u\n",
                     render_probe.video_pes_packets);
               printf("  vts_menu_render_private_stream_1_packets: %u\n",
                     render_probe.private_stream_1_packets);
               printf("  vts_menu_render_private_stream_2_packets: %u\n",
                     render_probe.private_stream_2_packets);
               printf("  vts_menu_render_subpicture_packets: %u\n",
                     render_probe.subpicture_packets);
               printf("  vts_menu_render_nav_pci_packets: %u\n",
                     render_probe.nav_pci_packets);
               printf("  vts_menu_render_nav_dsi_packets: %u\n",
                     render_probe.nav_dsi_packets);
               printf("  vts_menu_render_first_subpicture_stream_id: 0x%02x\n",
                     render_probe.first_subpicture_stream_id);
               printf("  vts_menu_render_starting_button: %u\n",
                     render_probe.starting_button);
               printf("  vts_menu_render_forced_select_button: %u\n",
                     render_probe.forced_select_button);
               printf("  vts_menu_render_forced_action_button: %u\n",
                     render_probe.forced_action_button);
               printf("  vts_menu_render_button_count: %u\n",
                     render_probe.button_count);
               printf("  vts_menu_render_pre_commands: %u\n",
                     render_probe.pre_command_count);
               printf("  vts_menu_render_post_commands: %u\n",
                     render_probe.post_command_count);
               printf("  vts_menu_render_cell_commands: %u\n",
                     render_probe.cell_command_count);
               printf("  vts_menu_render_parsed_post_commands: %u\n",
                     render_probe.parsed_post_command_count);
               if (render_probe.parsed_post_command_count)
               {
                  uint8_t command_index;

                  for (command_index = 0; command_index <
                        render_probe.parsed_post_command_count;
                        command_index++)
                     printf("  vts_menu_render_post_command_%u: "
                           "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                           (unsigned)(command_index + 1u),
                           render_probe.post_commands[command_index][0],
                           render_probe.post_commands[command_index][1],
                           render_probe.post_commands[command_index][2],
                           render_probe.post_commands[command_index][3],
                           render_probe.post_commands[command_index][4],
                           render_probe.post_commands[command_index][5],
                           render_probe.post_commands[command_index][6],
                           render_probe.post_commands[command_index][7]);
               }
               if (render_probe.button_count)
               {
                  uint8_t button_index;

                  for (button_index = 0; button_index <
                        render_probe.button_count; button_index++)
                  {
                     const struct deevee_dvd_menu_button *button =
                        &render_probe.buttons[button_index];

                     printf("  vts_menu_render_button_%u_rect: %u,%u-%u,%u\n",
                           (unsigned)(button_index + 1u),
                           button->x_start, button->y_start,
                           button->x_end, button->y_end);
                     printf("  vts_menu_render_button_%u_adjacent: "
                           "up=%u down=%u left=%u right=%u\n",
                           (unsigned)(button_index + 1u),
                           button->up, button->down, button->left,
                           button->right);
                     printf("  vts_menu_render_button_%u_auto_action: %s\n",
                           (unsigned)(button_index + 1u),
                           yes_no(button->auto_action));
                     printf("  vts_menu_render_button_%u_command: "
                           "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                           (unsigned)(button_index + 1u),
                           button->command[0], button->command[1],
                           button->command[2], button->command[3],
                           button->command[4], button->command[5],
                           button->command[6], button->command[7]);
                     {
                        struct deevee_dvd_playback_target button_target;
                        bool decoded;

                        memset(&button_target, 0, sizeof(button_target));
                        decoded = deevee_dvd_decode_playback_target_command(
                              button->command, 1,
                              DEEVEE_DVD_MENU_DOMAIN_VTS, &title_table,
                              &button_target);
                        printf("  vts_menu_render_button_%u_decoded: %s\n",
                              (unsigned)(button_index + 1u),
                              yes_no(decoded));
                        if (decoded)
                        {
                           printf("  vts_menu_render_button_%u_target_type: %s\n",
                                 (unsigned)(button_index + 1u),
                                 deevee_dvd_playback_target_type_name(
                                    button_target.type));
                           printf("  vts_menu_render_button_%u_target_domain: %s\n",
                                 (unsigned)(button_index + 1u),
                                 deevee_dvd_menu_domain_name(
                                    button_target.menu_domain));
                           printf("  vts_menu_render_button_%u_target_vts: %u\n",
                                 (unsigned)(button_index + 1u),
                                 button_target.vts_number);
                           printf("  vts_menu_render_button_%u_target_pgc: %u\n",
                                 (unsigned)(button_index + 1u),
                                 button_target.pgc_number);
                           printf("  vts_menu_render_button_%u_target_vts_title: %u\n",
                                 (unsigned)(button_index + 1u),
                                 button_target.vts_title_number);
                           if (button_target.type ==
                                 DEEVEE_DVD_PLAYBACK_TARGET_MENU)
                           {
                              struct deevee_dvd_menu_render_probe target_probe;
                              enum deevee_dvd_status target_status =
                                 DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

                              memset(&target_probe, 0, sizeof(target_probe));
                              if (button_target.menu_domain ==
                                    DEEVEE_DVD_MENU_DOMAIN_VTS &&
                                    button_target.vts_number &&
                                    button_target.pgc_number)
                                 target_status =
                                    deevee_dvd_probe_vts_menu_pgc_render_streams(
                                       &disc, button_target.vts_number,
                                       button_target.pgc_number - 1u,
                                       &target_probe);
                              else if (button_target.menu_domain ==
                                    DEEVEE_DVD_MENU_DOMAIN_VMG &&
                                    button_target.pgc_number)
                                 target_status =
                                    deevee_dvd_probe_vmgm_menu_pgc_render_streams(
                                       &disc, button_target.pgc_number - 1u,
                                       &target_probe);

                              printf("  vts_menu_render_button_%u_target_probe_status: %s\n",
                                    (unsigned)(button_index + 1u),
                                    deevee_dvd_status_name(target_status));
                              if (target_status == DEEVEE_DVD_OK)
                              {
                                 printf("  vts_menu_render_button_%u_target_probe_buttons: %u\n",
                                       (unsigned)(button_index + 1u),
                                       target_probe.button_count);
                                 printf("  vts_menu_render_button_%u_target_probe_pre_commands: %u\n",
                                       (unsigned)(button_index + 1u),
                                       target_probe.pre_command_count);
                                 printf("  vts_menu_render_button_%u_target_probe_post_commands: %u\n",
                                       (unsigned)(button_index + 1u),
                                       target_probe.post_command_count);
                                 printf("  vts_menu_render_button_%u_target_probe_parsed_post_commands: %u\n",
                                       (unsigned)(button_index + 1u),
                                       target_probe.parsed_post_command_count);
                                 printf("  vts_menu_render_button_%u_target_probe_has_video: %s\n",
                                       (unsigned)(button_index + 1u),
                                       yes_no(target_probe.has_video));
                                 printf("  vts_menu_render_button_%u_target_probe_has_nav: %s\n",
                                       (unsigned)(button_index + 1u),
                                       yes_no(target_probe.has_nav));
                                 if (target_probe.parsed_post_command_count)
                                 {
                                    uint8_t target_command_index;

                                    for (target_command_index = 0;
                                          target_command_index <
                                             target_probe.parsed_post_command_count;
                                          target_command_index++)
                                    {
                                       struct deevee_dvd_playback_target post_target;
                                       bool post_decoded;

                                       printf("  vts_menu_render_button_%u_target_probe_post_command_%u: "
                                             "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                             (unsigned)(button_index + 1u),
                                             (unsigned)(target_command_index + 1u),
                                             target_probe.post_commands[target_command_index][0],
                                             target_probe.post_commands[target_command_index][1],
                                             target_probe.post_commands[target_command_index][2],
                                             target_probe.post_commands[target_command_index][3],
                                             target_probe.post_commands[target_command_index][4],
                                             target_probe.post_commands[target_command_index][5],
                                             target_probe.post_commands[target_command_index][6],
                                             target_probe.post_commands[target_command_index][7]);
                                       memset(&post_target, 0,
                                             sizeof(post_target));
                                       post_decoded =
                                          deevee_dvd_decode_playback_target_command(
                                             target_probe.post_commands[target_command_index],
                                             button_target.vts_number,
                                             button_target.menu_domain,
                                             &title_table, &post_target);
                                       printf("  vts_menu_render_button_%u_target_probe_post_command_%u_decoded: %s\n",
                                             (unsigned)(button_index + 1u),
                                             (unsigned)(target_command_index + 1u),
                                             yes_no(post_decoded));
                                       if (post_decoded)
                                       {
                                          printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_type: %s\n",
                                                (unsigned)(button_index + 1u),
                                                (unsigned)(target_command_index + 1u),
                                                deevee_dvd_playback_target_type_name(
                                                   post_target.type));
                                          printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_domain: %s\n",
                                                (unsigned)(button_index + 1u),
                                                (unsigned)(target_command_index + 1u),
                                                deevee_dvd_menu_domain_name(
                                                   post_target.menu_domain));
                                          printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_vts: %u\n",
                                                (unsigned)(button_index + 1u),
                                                (unsigned)(target_command_index + 1u),
                                                post_target.vts_number);
                                          printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_pgc: %u\n",
                                                (unsigned)(button_index + 1u),
                                                (unsigned)(target_command_index + 1u),
                                                post_target.pgc_number);
                                          if (post_target.type ==
                                                DEEVEE_DVD_PLAYBACK_TARGET_MENU &&
                                                post_target.menu_domain ==
                                                   DEEVEE_DVD_MENU_DOMAIN_VTS &&
                                                post_target.vts_number)
                                          {
                                             struct deevee_dvd_menu_render_probe
                                                post_menu_probe;
                                             enum deevee_dvd_status
                                                post_menu_status =
                                                   DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;
                                             unsigned post_pgc_index = 0;

                                             memset(&post_menu_probe, 0,
                                                   sizeof(post_menu_probe));
                                             if (post_target.pgc_number)
                                             {
                                                post_pgc_index =
                                                   post_target.pgc_number - 1u;
                                                post_menu_status =
                                                   deevee_dvd_probe_vts_menu_pgc_render_streams(
                                                      &disc,
                                                      post_target.vts_number,
                                                      post_pgc_index,
                                                      &post_menu_probe);
                                             }
                                             else
                                             {
                                                for (post_pgc_index = 0;
                                                      post_pgc_index < 32u;
                                                      post_pgc_index++)
                                                {
                                                   post_menu_status =
                                                      deevee_dvd_probe_vts_menu_pgc_render_streams(
                                                         &disc,
                                                         post_target.vts_number,
                                                         post_pgc_index,
                                                         &post_menu_probe);
                                                   if (post_menu_status ==
                                                         DEEVEE_DVD_OK)
                                                      break;
                                                }
                                             }
                                             printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_probe_status: %s\n",
                                                   (unsigned)(button_index + 1u),
                                                   (unsigned)(target_command_index + 1u),
                                                   deevee_dvd_status_name(
                                                      post_menu_status));
                                             if (post_menu_status ==
                                                   DEEVEE_DVD_OK)
                                             {
                                                printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_probe_pgc: %u\n",
                                                      (unsigned)(button_index + 1u),
                                                      (unsigned)(target_command_index + 1u),
                                                      post_pgc_index + 1u);
                                                printf("  vts_menu_render_button_%u_target_probe_post_command_%u_target_probe_buttons: %u\n",
                                                      (unsigned)(button_index + 1u),
                                                      (unsigned)(target_command_index + 1u),
                                                      post_menu_probe.button_count);
                                             }
                                          }
                                       }
                                    }
                                 }
                              }
                           }
                        }
                     }
                  }
               }
               printf("  vts_menu_render_has_video: %s\n",
                     yes_no(render_probe.has_video));
               printf("  vts_menu_render_has_subpicture: %s\n",
                     yes_no(render_probe.has_subpicture));
               printf("  vts_menu_render_has_nav: %s\n",
                     yes_no(render_probe.has_nav));

               {
                  const uint8_t *resolved_command =
                     resolve_probe_button_command(&render_probe);
                  struct deevee_dvd_playback_target target;
                  struct deevee_dvd_title_pgc title_pgc;

                  memset(&target, 0, sizeof(target));
                  memset(&title_pgc, 0, sizeof(title_pgc));

                  printf("  resolved_menu_jump_command_present: %s\n",
                        yes_no(resolved_command != NULL));
                  if (resolved_command)
                  {
                     enum deevee_dvd_status target_status;
                     printf("  resolved_menu_jump_command: "
                           "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                           resolved_command[0], resolved_command[1],
                           resolved_command[2], resolved_command[3],
                           resolved_command[4], resolved_command[5],
                           resolved_command[6], resolved_command[7]);
                     printf("  resolved_menu_jump_target_status: %s\n",
                           yes_no(deevee_dvd_decode_playback_target_command(
                              resolved_command, 1,
                              DEEVEE_DVD_MENU_DOMAIN_VTS, &title_table,
                              &target)));
                     if (target.type == DEEVEE_DVD_PLAYBACK_TARGET_TITLE)
                     {
                        printf("  resolved_menu_jump_target_vts: %u\n",
                              target.vts_number);
                        printf("  resolved_menu_jump_target_vts_title: %u\n",
                              target.vts_title_number);
                        printf("  resolved_menu_jump_target_ptt: %u\n",
                              target.ptt_number);
                        target_status = deevee_dvd_resolve_title_pgc(&disc,
                              &target, &title_pgc);
                        printf("  resolved_title_pgc_status: %s\n",
                              deevee_dvd_status_name(target_status));
                        if (target_status == DEEVEE_DVD_OK)
                        {
                           printf("  resolved_title_pgc_vts: %u\n",
                                 title_pgc.vts_number);
                           printf("  resolved_title_pgc_vts_title: %u\n",
                                 title_pgc.vts_title_number);
                           printf("  resolved_title_pgc_ptt: %u\n",
                                 title_pgc.ptt_number);
                           printf("  resolved_title_pgc_number: %u\n",
                                 title_pgc.pgc_number);
                           printf("  resolved_title_pgc_program: %u\n",
                                 title_pgc.program_number);
                           printf("  resolved_title_pgc_programs: %u\n",
                                 title_pgc.program_count);
                           printf("  resolved_title_pgc_cells: %u\n",
                                 title_pgc.cell_count);
                           printf("  resolved_title_pgc_first_cell: %u\n",
                                 title_pgc.first_cell);
                           printf("  resolved_title_pgc_last_cell: %u\n",
                                 title_pgc.last_cell);
                           printf("  resolved_title_pgc_first_sector: %u\n",
                                 title_pgc.first_sector);
                           printf("  resolved_title_pgc_last_sector: %u\n",
                                 title_pgc.last_sector);
                           printf("  resolved_title_pgc_sector_count: %u\n",
                                 title_pgc.sector_count);
                           print_title_packet_probe(&disc, &title_pgc);
                           print_title_decode_probe(&disc, &title_pgc);
                           print_title_audio_probe(&disc, &title_pgc);
                        }
                     }
                  }
               }

               {
                  unsigned candidate_pgc;

                  for (candidate_pgc = 0; candidate_pgc < 4u;
                        candidate_pgc++)
                  {
                     struct deevee_dvd_menu_render_probe candidate_probe;
                     enum deevee_dvd_status candidate_status;

                     memset(&candidate_probe, 0, sizeof(candidate_probe));
                     candidate_status =
                        deevee_dvd_probe_vts_menu_pgc_render_streams(&disc,
                              1, candidate_pgc, &candidate_probe);
                     printf("  vts_1_menu_pgc_%u_probe_status: %s\n",
                           candidate_pgc + 1u,
                           deevee_dvd_status_name(candidate_status));
                     if (candidate_status == DEEVEE_DVD_OK)
                     {
                        printf("  vts_1_menu_pgc_%u_buttons: %u\n",
                              candidate_pgc + 1u,
                              candidate_probe.button_count);
                        printf("  vts_1_menu_pgc_%u_post_commands: %u\n",
                              candidate_pgc + 1u,
                              candidate_probe.post_command_count);
                        if (candidate_probe.parsed_post_command_count)
                           printf("  vts_1_menu_pgc_%u_post_command_1: "
                                 "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                 candidate_pgc + 1u,
                                 candidate_probe.post_commands[0][0],
                                 candidate_probe.post_commands[0][1],
                                 candidate_probe.post_commands[0][2],
                                 candidate_probe.post_commands[0][3],
                                 candidate_probe.post_commands[0][4],
                                 candidate_probe.post_commands[0][5],
                                 candidate_probe.post_commands[0][6],
                                 candidate_probe.post_commands[0][7]);
                        printf("  vts_1_menu_pgc_%u_has_video: %s\n",
                              candidate_pgc + 1u,
                              yes_no(candidate_probe.has_video));
                     }
                  }

                  for (candidate_pgc = 0; candidate_pgc < 4u;
                        candidate_pgc++)
                  {
                     struct deevee_dvd_menu_render_probe candidate_probe;
                     enum deevee_dvd_status candidate_status;

                     memset(&candidate_probe, 0, sizeof(candidate_probe));
                     candidate_status =
                        deevee_dvd_probe_vts_menu_pgc_render_streams(&disc,
                              2, candidate_pgc, &candidate_probe);
                     printf("  vts_2_menu_pgc_%u_probe_status: %s\n",
                           candidate_pgc + 1u,
                           deevee_dvd_status_name(candidate_status));
                     if (candidate_status == DEEVEE_DVD_OK)
                     {
                        size_t button_index;

                        printf("  vts_2_menu_pgc_%u_buttons: %u\n",
                              candidate_pgc + 1u,
                              candidate_probe.button_count);
                        printf("  vts_2_menu_pgc_%u_has_video: %s\n",
                              candidate_pgc + 1u,
                              yes_no(candidate_probe.has_video));
                        for (button_index = 0;
                              button_index < candidate_probe.button_count;
                              button_index++)
                        {
                           const struct deevee_dvd_menu_button *button =
                              &candidate_probe.buttons[button_index];
                           struct deevee_dvd_playback_target candidate_target;
                           bool decoded;

                           memset(&candidate_target, 0,
                                 sizeof(candidate_target));
                           decoded =
                              deevee_dvd_decode_playback_target_command(
                                    button->command, 2,
                                    DEEVEE_DVD_MENU_DOMAIN_VTS,
                                    &title_table, &candidate_target);
                           printf("  vts_2_menu_pgc_%u_button_%u_command: "
                                 "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                                 candidate_pgc + 1u,
                                 (unsigned)(button_index + 1u),
                                 button->command[0], button->command[1],
                                 button->command[2], button->command[3],
                                 button->command[4], button->command[5],
                                 button->command[6], button->command[7]);
                           printf("  vts_2_menu_pgc_%u_button_%u_decoded: %s\n",
                                 candidate_pgc + 1u,
                                 (unsigned)(button_index + 1u),
                                 yes_no(decoded));
                           if (decoded)
                           {
                              printf("  vts_2_menu_pgc_%u_button_%u_target_type: %s\n",
                                    candidate_pgc + 1u,
                                    (unsigned)(button_index + 1u),
                                    deevee_dvd_playback_target_type_name(
                                       candidate_target.type));
                              printf("  vts_2_menu_pgc_%u_button_%u_target_vts: %u\n",
                                    candidate_pgc + 1u,
                                    (unsigned)(button_index + 1u),
                                    candidate_target.vts_number);
                              printf("  vts_2_menu_pgc_%u_button_%u_target_vts_title: %u\n",
                                    candidate_pgc + 1u,
                                    (unsigned)(button_index + 1u),
                                    candidate_target.vts_title_number);
                           }
                        }
                     }
                  }
               }
            }
         }
      }
   }

   deevee_disc_close(&disc);
}

static int probe_one(const char *path)
{
   probe_stat_type st;
   struct deevee_content_info info;
   int exists;
   int is_directory = 0;
   int is_regular_file = 0;
   int accepted;

   memset(&info, 0, sizeof(info));

   exists = probe_stat_path(path, &st);
   if (exists)
   {
      is_directory = S_ISDIR(st.st_mode);
      is_regular_file = S_ISREG(st.st_mode);
   }

   accepted = deevee_content_probe(path, &info);

   printf("path: %s\n", path);
   printf("  decoder_backend: %s\n", deevee_decoder_backend_name());
   printf("  decoder_available: %s\n",
         yes_no(deevee_decoder_is_available()));
   printf("  exists: %s\n", yes_no(exists));
   printf("  directory: %s\n", yes_no(is_directory));
   printf("  regular_file: %s\n", yes_no(is_regular_file));
   printf("  accepted: %s\n", yes_no(accepted));
   printf("  detected_type: %s\n",
         accepted ? deevee_content_type_name(info.type) : "unsupported");
   if (accepted)
   {
      print_dvdnav_probe(&info);
      print_disc_probe(&info);
   }

   return accepted ? 0 : 1;
}

int main(int argc, char **argv)
{
   int i;
   int failures = 0;

   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <disc.iso|disc.chd> [...]\n",
            argv[0]);
      return 2;
   }

   for (i = 1; i < argc; i++)
   {
      if (i > 1)
         puts("");
      failures += probe_one(argv[i]) != 0;
   }

   return failures ? 1 : 0;
}
