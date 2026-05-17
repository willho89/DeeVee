#include "deevee_content.h"
#include "deevee_decoder.h"
#include "deevee_disc.h"
#include "deevee_dvd.h"
#include "deevee_iso.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

static void print_disc_probe(const struct deevee_content_info *info)
{
   struct deevee_disc disc;
   enum deevee_disc_status status;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];

   deevee_disc_init(&disc);
   status = deevee_disc_open(&disc, info);
   printf("  reader_status: %s\n", deevee_disc_status_name(status));

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
                  const struct deevee_dvd_menu_button *button =
                     &render_probe.buttons[0];

                  printf("  vts_menu_render_button_1_rect: %u,%u-%u,%u\n",
                        button->x_start, button->y_start,
                        button->x_end, button->y_end);
                  printf("  vts_menu_render_button_1_adjacent: "
                        "up=%u down=%u left=%u right=%u\n",
                        button->up, button->down, button->left,
                        button->right);
                  printf("  vts_menu_render_button_1_auto_action: %s\n",
                        yes_no(button->auto_action));
                  printf("  vts_menu_render_button_1_command: "
                        "%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        button->command[0], button->command[1],
                        button->command[2], button->command[3],
                        button->command[4], button->command[5],
                        button->command[6], button->command[7]);
               }
               printf("  vts_menu_render_has_video: %s\n",
                     yes_no(render_probe.has_video));
               printf("  vts_menu_render_has_subpicture: %s\n",
                     yes_no(render_probe.has_subpicture));
               printf("  vts_menu_render_has_nav: %s\n",
                     yes_no(render_probe.has_nav));
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
      print_disc_probe(&info);

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
