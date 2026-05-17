#include "deevee_dvd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t read_be16(const uint8_t *data)
{
   return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
   return ((uint32_t)data[0] << 24) |
      ((uint32_t)data[1] << 16) |
      ((uint32_t)data[2] << 8) |
      (uint32_t)data[3];
}

static int is_start_code(const uint8_t *data)
{
   return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01;
}

static int read_private_stream_1_substream(const uint8_t *sector,
      size_t offset, uint8_t *substream_id)
{
   size_t payload_offset;

   if (!sector || !substream_id || offset + 10 > DEEVEE_DVD_SECTOR_SIZE)
      return 0;

   payload_offset = offset + 9 + sector[offset + 8];
   if (payload_offset >= DEEVEE_DVD_SECTOR_SIZE)
      return 0;

   *substream_id = sector[payload_offset];
   return 1;
}

static int read_private_stream_2_substream(const uint8_t *sector,
      size_t offset, uint8_t *substream_id)
{
   if (!sector || !substream_id || offset + 7 > DEEVEE_DVD_SECTOR_SIZE)
      return 0;

   *substream_id = sector[offset + 6];
   return 1;
}

static int read_pes_payload_bounds(const uint8_t *sector, size_t offset,
      size_t *payload_offset, size_t *payload_size)
{
   uint16_t packet_length;
   size_t header_data_length;
   size_t start;
   size_t end;

   if (!sector || !payload_offset || !payload_size ||
         offset + 9 > DEEVEE_DVD_SECTOR_SIZE)
      return 0;

   packet_length = read_be16(sector + offset + 4);
   header_data_length = sector[offset + 8];
   start = offset + 9 + header_data_length;
   if (start >= DEEVEE_DVD_SECTOR_SIZE)
      return 0;

   if (packet_length)
   {
      size_t pes_end = offset + 6 + packet_length;
      end = pes_end < DEEVEE_DVD_SECTOR_SIZE ? pes_end : DEEVEE_DVD_SECTOR_SIZE;
   }
   else
      end = DEEVEE_DVD_SECTOR_SIZE;

   if (end <= start)
      return 0;

   *payload_offset = start;
   *payload_size = end - start;
   return 1;
}

static int read_pes_payload_bounds_from_buffer(const uint8_t *data,
      size_t data_size, size_t offset, size_t *payload_offset,
      size_t *payload_size, size_t *packet_end)
{
   uint16_t packet_length;
   size_t header_data_length;
   size_t start;
   size_t end;

   if (!data || !payload_offset || !payload_size || !packet_end ||
         offset + 9 > data_size)
      return 0;

   packet_length = read_be16(data + offset + 4);
   header_data_length = data[offset + 8];
   start = offset + 9 + header_data_length;
   if (start >= data_size)
      return 0;

   if (packet_length)
   {
      end = offset + 6 + packet_length;
      if (end > data_size)
         return 0;
   }
   else
   {
      end = start;
      while (end + 4 <= data_size)
      {
         if (end > start && is_start_code(data + end))
            break;
         end++;
      }
      if (end <= start)
         end = data_size;
   }

   if (end <= start)
      return 0;

   *payload_offset = start;
   *payload_size = end - start;
   *packet_end = end;
   return 1;
}

static enum deevee_dvd_status walk_video_payloads_in_buffer(
      const uint8_t *data, size_t data_size,
      deevee_dvd_video_payload_callback callback, void *user_data)
{
   size_t offset = 0;

   if (!data || !callback)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   while (offset + 9 <= data_size)
   {
      uint8_t stream_id;
      size_t payload_offset;
      size_t payload_size;
      size_t packet_end;

      if (!is_start_code(data + offset))
      {
         offset++;
         continue;
      }

      stream_id = data[offset + 3];
      if (stream_id >= 0xe0 && stream_id <= 0xef &&
            read_pes_payload_bounds_from_buffer(data, data_size, offset,
               &payload_offset, &payload_size, &packet_end))
      {
         if (!callback(data + payload_offset, payload_size, user_data))
            return DEEVEE_DVD_OK;

         offset = packet_end;
         continue;
      }

      if (stream_id != 0xba && stream_id != 0xb9 &&
            read_pes_payload_bounds_from_buffer(data, data_size, offset,
               &payload_offset, &payload_size, &packet_end))
      {
         offset = packet_end;
         continue;
      }

      offset++;
   }

   return DEEVEE_DVD_OK;
}

static void parse_sequence_header(const uint8_t *data,
      struct deevee_dvd_video_probe *probe)
{
   if (!data || !probe)
      return;

   probe->sequence_width = (uint16_t)(((uint16_t)data[0] << 4) |
         (data[1] >> 4));
   probe->sequence_height = (uint16_t)(((uint16_t)(data[1] & 0x0f) << 8) |
         data[2]);
   probe->aspect_ratio_code = data[3] >> 4;
   probe->frame_rate_code = data[3] & 0x0f;
   probe->bit_rate_value = ((uint32_t)data[4] << 10) |
      ((uint32_t)data[5] << 2) | (data[6] >> 6);
   probe->vbv_buffer_size_value = (uint16_t)(((uint16_t)(data[6] & 0x1f) << 5) |
         (data[7] >> 3));
   probe->constrained_parameters_flag = (data[7] & 0x04) != 0;
   probe->has_sequence_header = true;
}

static void find_sequence_header(const uint8_t *payload, size_t payload_size,
      struct deevee_dvd_video_probe *probe)
{
   size_t i;

   if (!payload || !probe || probe->has_sequence_header)
      return;

   for (i = 0; i + 12 <= payload_size; i++)
   {
      if (is_start_code(payload + i) && payload[i + 3] == 0xb3)
      {
         parse_sequence_header(payload + i + 4, probe);
         return;
      }
   }
}

static enum deevee_dvd_status read_file_bytes(struct deevee_disc *disc,
      const struct deevee_iso_entry *entry, uint32_t byte_offset,
      uint8_t *out, size_t out_size)
{
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   size_t copied = 0;

   if (!disc || !entry || !out || !out_size)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   if (byte_offset >= entry->size)
      return DEEVEE_DVD_ERROR_READ_FAILED;

   while (copied < out_size && (uint64_t)byte_offset + copied < entry->size)
   {
      uint64_t file_offset = (uint64_t)byte_offset + copied;
      uint64_t lba = entry->lba + file_offset / DEEVEE_DVD_SECTOR_SIZE;
      size_t sector_offset = (size_t)(file_offset % DEEVEE_DVD_SECTOR_SIZE);
      size_t available = DEEVEE_DVD_SECTOR_SIZE - sector_offset;
      size_t remaining = out_size - copied;
      uint64_t file_remaining = entry->size - file_offset;
      size_t to_copy = available < remaining ? available : remaining;

      if ((uint64_t)to_copy > file_remaining)
         to_copy = (size_t)file_remaining;

      if (deevee_disc_read_sector(disc, lba, sector, sizeof(sector)) !=
            DEEVEE_DISC_OK)
         return DEEVEE_DVD_ERROR_READ_FAILED;

      memcpy(out + copied, sector + sector_offset, to_copy);
      copied += to_copy;
   }

   return copied ? DEEVEE_DVD_OK : DEEVEE_DVD_ERROR_READ_FAILED;
}

static enum deevee_dvd_status read_file_start(struct deevee_disc *disc,
      const struct deevee_iso_entry *entry, uint8_t *out, size_t out_size)
{
   return read_file_bytes(disc, entry, 0, out, out_size);
}

enum deevee_dvd_status deevee_dvd_probe(struct deevee_disc *disc,
      struct deevee_dvd_info *info)
{
   enum deevee_iso_status iso_status;
   uint8_t ifo_start[DEEVEE_DVD_IFO_PROBE_BYTES];
   enum deevee_dvd_status read_status;

   if (!disc || !info)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(info, 0, sizeof(*info));

   iso_status = deevee_iso_find_path(disc,
         "/VIDEO_TS/VIDEO_TS.IFO", &info->video_ts_ifo);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_IFO_NOT_FOUND;

   memset(ifo_start, 0, sizeof(ifo_start));
   read_status = read_file_start(disc, &info->video_ts_ifo,
         ifo_start, sizeof(ifo_start));
   if (read_status != DEEVEE_DVD_OK)
      return read_status;

   memcpy(info->vmg_identifier, ifo_start, 12);
   info->vmg_identifier[12] = '\0';
   info->is_dvd_video = memcmp(ifo_start, DEEVEE_DVD_VMG_IDENTIFIER, 12) == 0;
   info->vmg_last_sector = read_be32(ifo_start + 0x0c);
   info->vmgi_last_sector = read_be32(ifo_start + 0x1c);
   info->vmg_title_set_count = read_be16(ifo_start + 0x3e);
   info->vmgi_last_byte = read_be32(ifo_start + 0x80);
   info->first_play_pgc = read_be32(ifo_start + 0x84);
   info->vmgm_vobs = read_be32(ifo_start + 0xc0);
   info->tt_srpt = read_be32(ifo_start + 0xc4);
   info->vmgm_pgci_ut = read_be32(ifo_start + 0xc8);
   info->ptl_mait = read_be32(ifo_start + 0xcc);
   info->vts_atrt = read_be32(ifo_start + 0xd0);
   info->txtdt_mgi = read_be32(ifo_start + 0xd4);
   info->vmgm_c_adt = read_be32(ifo_start + 0xd8);
   info->vmgm_vobu_admap = read_be32(ifo_start + 0xdc);

   return info->is_dvd_video ? DEEVEE_DVD_OK : DEEVEE_DVD_ERROR_INVALID_VMG;
}

enum deevee_dvd_status deevee_dvd_read_title_table(struct deevee_disc *disc,
      const struct deevee_dvd_info *info,
      struct deevee_dvd_title_table *table)
{
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint16_t i;
   uint16_t parsed_count;

   if (!disc || !info || !table)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(table, 0, sizeof(*table));

   if (!info->is_dvd_video || !info->tt_srpt)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   if (deevee_disc_read_sector(disc,
            (uint64_t)info->video_ts_ifo.lba + info->tt_srpt,
            sector, sizeof(sector)) != DEEVEE_DISC_OK)
      return DEEVEE_DVD_ERROR_READ_FAILED;

   table->title_count = read_be16(sector);
   table->last_byte = read_be32(sector + 4);
   parsed_count = table->title_count;
   if (parsed_count > DEEVEE_DVD_MAX_TITLES)
      parsed_count = DEEVEE_DVD_MAX_TITLES;

   if ((size_t)8 + (size_t)parsed_count * 12 > sizeof(sector))
      return DEEVEE_DVD_ERROR_MALFORMED;

   table->parsed_title_count = parsed_count;
   for (i = 0; i < parsed_count; i++)
   {
      const uint8_t *entry = sector + 8 + (size_t)i * 12;
      table->titles[i].title_type = entry[0];
      table->titles[i].angle_count = entry[1];
      table->titles[i].chapter_count = read_be16(entry + 2);
      table->titles[i].parental_management_mask = read_be16(entry + 4);
      table->titles[i].vts_number = entry[6];
      table->titles[i].vts_title_number = entry[7];
      table->titles[i].vts_start_sector = read_be32(entry + 8);
   }

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_read_menu_language_table(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_language_table *table)
{
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint16_t i;
   uint16_t parsed_count;

   if (!disc || !info || !table)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(table, 0, sizeof(*table));

   if (!info->is_dvd_video || !info->vmgm_pgci_ut)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   if (deevee_disc_read_sector(disc,
            (uint64_t)info->video_ts_ifo.lba + info->vmgm_pgci_ut,
            sector, sizeof(sector)) != DEEVEE_DISC_OK)
      return DEEVEE_DVD_ERROR_READ_FAILED;

   table->language_count = read_be16(sector);
   table->last_byte = read_be32(sector + 4);
   parsed_count = table->language_count;
   if (parsed_count > DEEVEE_DVD_MAX_MENU_LANGUAGE_UNITS)
      parsed_count = DEEVEE_DVD_MAX_MENU_LANGUAGE_UNITS;

   if ((size_t)8 + (size_t)parsed_count * 8 > sizeof(sector))
      return DEEVEE_DVD_ERROR_MALFORMED;

   table->parsed_language_count = parsed_count;
   for (i = 0; i < parsed_count; i++)
   {
      const uint8_t *entry = sector + 8 + (size_t)i * 8;
      table->languages[i].language[0] = (char)entry[0];
      table->languages[i].language[1] = (char)entry[1];
      table->languages[i].language[2] = '\0';
      table->languages[i].language_extension = entry[2];
      table->languages[i].menu_existence = entry[3];
      table->languages[i].start_byte = read_be32(entry + 4);
   }

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_read_first_menu_pgc(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_pgc_summary *pgc)
{
   enum deevee_dvd_status status;
   struct deevee_dvd_menu_language_table menu_table;
   const struct deevee_dvd_menu_language_unit *language;
   uint8_t language_unit_header[16];
   uint8_t pgc_header[0xec];
   uint32_t table_byte_offset;
   uint32_t language_unit_byte_offset;
   uint32_t pgc_byte_offset;

   if (!disc || !info || !pgc)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(pgc, 0, sizeof(*pgc));

   status = deevee_dvd_read_menu_language_table(disc, info, &menu_table);
   if (status != DEEVEE_DVD_OK)
      return status;

   if (!menu_table.parsed_language_count)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   language = &menu_table.languages[0];
   table_byte_offset = info->vmgm_pgci_ut * DEEVEE_DVD_SECTOR_SIZE;
   language_unit_byte_offset = table_byte_offset + language->start_byte;

   status = read_file_bytes(disc, &info->video_ts_ifo,
         language_unit_byte_offset, language_unit_header,
         sizeof(language_unit_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   pgc->language[0] = language->language[0];
   pgc->language[1] = language->language[1];
   pgc->language[2] = '\0';
   pgc->language_unit_start_byte = language->start_byte;
   pgc->pgc_count = read_be16(language_unit_header);
   pgc->language_unit_last_byte = read_be32(language_unit_header + 4);
   if (!pgc->pgc_count)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   pgc->pgc_category = read_be32(language_unit_header + 8);
   pgc->pgc_start_byte = read_be32(language_unit_header + 12);
   pgc_byte_offset = language_unit_byte_offset + pgc->pgc_start_byte;

   status = read_file_bytes(disc, &info->video_ts_ifo,
         pgc_byte_offset, pgc_header, sizeof(pgc_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   pgc->program_count = pgc_header[2];
   pgc->cell_count = pgc_header[3];
   memcpy(pgc->playback_time, pgc_header + 4, sizeof(pgc->playback_time));
   pgc->prohibited_user_ops = read_be32(pgc_header + 8);
   pgc->command_table_offset = read_be16(pgc_header + 0xe4);
   pgc->program_map_offset = read_be16(pgc_header + 0xe6);
   pgc->cell_playback_table_offset = read_be16(pgc_header + 0xe8);
   pgc->cell_position_table_offset = read_be16(pgc_header + 0xea);

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_read_first_menu_pgc_tables(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_pgc_table_summary *tables)
{
   enum deevee_dvd_status status;
   struct deevee_dvd_menu_pgc_summary pgc;
   uint8_t command_table[8];
   uint8_t program_entry;
   uint8_t cell_playback[24];
   uint8_t cell_position[4];
   uint32_t pgc_byte_offset;

   if (!disc || !info || !tables)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(tables, 0, sizeof(*tables));

   status = deevee_dvd_read_first_menu_pgc(disc, info, &pgc);
   if (status != DEEVEE_DVD_OK)
      return status;

   if (!pgc.command_table_offset || !pgc.program_map_offset ||
         !pgc.cell_playback_table_offset || !pgc.cell_position_table_offset ||
         !pgc.program_count || !pgc.cell_count)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   tables->language[0] = pgc.language[0];
   tables->language[1] = pgc.language[1];
   tables->language[2] = '\0';
   pgc_byte_offset = info->vmgm_pgci_ut * DEEVEE_DVD_SECTOR_SIZE +
      pgc.language_unit_start_byte + pgc.pgc_start_byte;

   status = read_file_bytes(disc, &info->video_ts_ifo,
         pgc_byte_offset + pgc.command_table_offset,
         command_table, sizeof(command_table));
   if (status != DEEVEE_DVD_OK)
      return status;

   tables->pre_command_count = read_be16(command_table);
   tables->post_command_count = read_be16(command_table + 2);
   tables->cell_command_count = read_be16(command_table + 4);
   tables->command_table_last_byte = read_be16(command_table + 6);

   status = read_file_bytes(disc, &info->video_ts_ifo,
         pgc_byte_offset + pgc.program_map_offset,
         &program_entry, sizeof(program_entry));
   if (status != DEEVEE_DVD_OK)
      return status;
   tables->first_program_entry_cell = program_entry;

   status = read_file_bytes(disc, &info->video_ts_ifo,
         pgc_byte_offset + pgc.cell_playback_table_offset,
         cell_playback, sizeof(cell_playback));
   if (status != DEEVEE_DVD_OK)
      return status;

   tables->first_cell_category = read_be32(cell_playback);
   memcpy(tables->first_cell_playback_time, cell_playback + 4,
         sizeof(tables->first_cell_playback_time));
   tables->first_cell_first_vobu_start_sector = read_be32(cell_playback + 8);
   tables->first_cell_first_ilvu_end_sector = read_be32(cell_playback + 12);
   tables->first_cell_last_vobu_start_sector = read_be32(cell_playback + 16);
   tables->first_cell_last_vobu_end_sector = read_be32(cell_playback + 20);

   status = read_file_bytes(disc, &info->video_ts_ifo,
         pgc_byte_offset + pgc.cell_position_table_offset,
         cell_position, sizeof(cell_position));
   if (status != DEEVEE_DVD_OK)
      return status;

   tables->first_cell_vob_id = read_be16(cell_position);
   tables->first_cell_id = cell_position[3];

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_resolve_first_menu_vob_span(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_vob_span *span)
{
   enum deevee_iso_status iso_status;
   enum deevee_dvd_status status;
   struct deevee_dvd_menu_pgc_table_summary tables;

   if (!disc || !info || !span)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(span, 0, sizeof(*span));

   iso_status = deevee_iso_find_path(disc,
         "/VIDEO_TS/VIDEO_TS.VOB", &span->vmgm_vob);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   if (span->vmgm_vob.is_directory || !span->vmgm_vob.size)
      return DEEVEE_DVD_ERROR_MALFORMED;

   status = deevee_dvd_read_first_menu_pgc_tables(disc, info, &tables);
   if (status != DEEVEE_DVD_OK)
      return status;

   span->vmgm_vob_sector_count =
      (span->vmgm_vob.size + DEEVEE_DVD_SECTOR_SIZE - 1) /
      DEEVEE_DVD_SECTOR_SIZE;
   span->first_cell_start_sector =
      tables.first_cell_first_vobu_start_sector;
   span->first_cell_end_sector =
      tables.first_cell_last_vobu_end_sector;
   span->first_cell_vob_id = tables.first_cell_vob_id;
   span->first_cell_id = tables.first_cell_id;

   if (span->first_cell_start_sector > span->first_cell_end_sector ||
         span->first_cell_end_sector >= span->vmgm_vob_sector_count)
      return DEEVEE_DVD_ERROR_MALFORMED;

   span->first_cell_start_lba = span->vmgm_vob.lba +
      span->first_cell_start_sector;
   span->first_cell_end_lba = span->vmgm_vob.lba +
      span->first_cell_end_sector;

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_probe_first_menu_vob_packets(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_vob_packet_probe *probe)
{
   enum deevee_dvd_status status;
   struct deevee_dvd_menu_vob_span span;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint64_t lba;

   if (!disc || !info || !probe)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(probe, 0, sizeof(*probe));

   status = deevee_dvd_resolve_first_menu_vob_span(disc, info, &span);
   if (status != DEEVEE_DVD_OK)
      return status;

   for (lba = span.first_cell_start_lba; lba <= span.first_cell_end_lba; lba++)
   {
      size_t offset;

      if (deevee_disc_read_sector(disc, lba, sector, sizeof(sector)) !=
            DEEVEE_DISC_OK)
         return DEEVEE_DVD_ERROR_READ_FAILED;

      probe->scanned_sectors++;

      for (offset = 0; offset + 4 <= sizeof(sector); offset++)
      {
         uint8_t stream_id;

         if (!is_start_code(sector + offset))
            continue;

         stream_id = sector[offset + 3];
         switch (stream_id)
         {
            case 0xba:
               probe->pack_header_count++;
               probe->has_pack_header = true;
               break;
            case 0xbb:
               probe->system_header_count++;
               break;
            case 0xbc:
               probe->program_stream_map_count++;
               break;
            case 0xbd:
               probe->private_stream_1_count++;
               {
                  uint8_t substream_id;
                  if (read_private_stream_1_substream(sector, offset,
                           &substream_id))
                  {
                     if (!probe->first_private_stream_1_substream_id)
                        probe->first_private_stream_1_substream_id =
                           substream_id;

                     if (substream_id >= 0x80 && substream_id <= 0x87)
                     {
                        probe->ac3_audio_count++;
                        probe->has_audio = true;
                        if (!probe->first_audio_stream_id)
                           probe->first_audio_stream_id = substream_id;
                     }
                     else if (substream_id >= 0x88 && substream_id <= 0x8f)
                     {
                        probe->dts_audio_count++;
                        probe->has_audio = true;
                        if (!probe->first_audio_stream_id)
                           probe->first_audio_stream_id = substream_id;
                     }
                     else if (substream_id >= 0xa0 && substream_id <= 0xa7)
                     {
                        probe->lpcm_audio_count++;
                        probe->has_audio = true;
                        if (!probe->first_audio_stream_id)
                           probe->first_audio_stream_id = substream_id;
                     }
                     else if (substream_id >= 0x20 && substream_id <= 0x3f)
                     {
                        probe->subpicture_count++;
                        probe->has_subpicture = true;
                     }
                     else
                        probe->private_stream_1_unknown_count++;
                  }
                  else
                     probe->private_stream_1_unknown_count++;
               }
               break;
            case 0xbe:
               probe->padding_stream_count++;
               break;
            case 0xbf:
               probe->private_stream_2_count++;
               probe->nav_pack_count++;
               probe->has_nav = true;
               {
                  uint8_t substream_id;
                  if (read_private_stream_2_substream(sector, offset,
                           &substream_id))
                  {
                     if (probe->private_stream_2_count == 1)
                        probe->first_nav_substream_id = substream_id;

                     if (substream_id == 0x00)
                        probe->nav_pci_count++;
                     else if (substream_id == 0x01)
                        probe->nav_dsi_count++;
                     else
                        probe->private_stream_2_unknown_count++;
                  }
                  else
                     probe->private_stream_2_unknown_count++;
               }
               break;
            default:
               if (stream_id >= 0xe0 && stream_id <= 0xef)
               {
                  probe->video_pes_count++;
                  probe->has_video = true;
                  if (!probe->first_video_stream_id)
                     probe->first_video_stream_id = stream_id;
               }
               else if (stream_id >= 0xc0 && stream_id <= 0xdf)
               {
                  probe->audio_pes_count++;
                  probe->has_audio = true;
                  if (!probe->first_audio_stream_id)
                     probe->first_audio_stream_id = stream_id;
               }
               else
                  probe->other_pes_count++;
               break;
         }
      }
   }

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_probe_first_menu_video(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_video_probe *probe)
{
   enum deevee_dvd_status status;
   struct deevee_dvd_menu_vob_span span;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint64_t lba;

   if (!disc || !info || !probe)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(probe, 0, sizeof(*probe));

   status = deevee_dvd_resolve_first_menu_vob_span(disc, info, &span);
   if (status != DEEVEE_DVD_OK)
      return status;

   for (lba = span.first_cell_start_lba; lba <= span.first_cell_end_lba; lba++)
   {
      size_t offset;

      if (deevee_disc_read_sector(disc, lba, sector, sizeof(sector)) !=
            DEEVEE_DISC_OK)
         return DEEVEE_DVD_ERROR_READ_FAILED;

      probe->scanned_sectors++;

      for (offset = 0; offset + 9 <= sizeof(sector); offset++)
      {
         uint8_t stream_id;
         size_t payload_offset;
         size_t payload_size;

         if (!is_start_code(sector + offset))
            continue;

         stream_id = sector[offset + 3];
         if (stream_id < 0xe0 || stream_id > 0xef)
            continue;

         if (!read_pes_payload_bounds(sector, offset, &payload_offset,
                  &payload_size))
            continue;

         probe->video_pes_packets++;
         probe->video_payload_bytes += (uint32_t)payload_size;
         probe->has_video_payload = true;
         if (!probe->first_video_stream_id)
            probe->first_video_stream_id = stream_id;

         find_sequence_header(sector + payload_offset, payload_size, probe);
      }
   }

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_walk_first_menu_video_payloads(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      deevee_dvd_video_payload_callback callback, void *user_data)
{
   enum deevee_dvd_status status;
   struct deevee_dvd_menu_vob_span span;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint64_t lba;

   if (!disc || !info || !callback)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   status = deevee_dvd_resolve_first_menu_vob_span(disc, info, &span);
   if (status != DEEVEE_DVD_OK)
      return status;

   for (lba = span.first_cell_start_lba; lba <= span.first_cell_end_lba; lba++)
   {
      size_t offset;

      if (deevee_disc_read_sector(disc, lba, sector, sizeof(sector)) !=
            DEEVEE_DISC_OK)
         return DEEVEE_DVD_ERROR_READ_FAILED;

      for (offset = 0; offset + 9 <= sizeof(sector); offset++)
      {
         uint8_t stream_id;
         size_t payload_offset;
         size_t payload_size;

         if (!is_start_code(sector + offset))
            continue;

         stream_id = sector[offset + 3];
         if (stream_id < 0xe0 || stream_id > 0xef)
            continue;

         if (!read_pes_payload_bounds(sector, offset, &payload_offset,
                  &payload_size))
            continue;

         if (!callback(sector + payload_offset, payload_size, user_data))
            return DEEVEE_DVD_OK;
      }
   }

   return DEEVEE_DVD_OK;
}

enum deevee_dvd_status deevee_dvd_walk_vob_video_payloads(
      struct deevee_disc *disc, const char *iso_path,
      deevee_dvd_video_payload_callback callback, void *user_data)
{
   enum deevee_iso_status iso_status;
   struct deevee_iso_entry entry;
   uint8_t *vob_data;
   enum deevee_dvd_status status;

   if (!disc || !iso_path || !callback)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   iso_status = deevee_iso_find_path(disc, iso_path, &entry);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_IFO_NOT_FOUND;

   if (entry.is_directory || !entry.size)
      return DEEVEE_DVD_ERROR_MALFORMED;

   vob_data = (uint8_t *)malloc(entry.size);
   if (!vob_data)
      return DEEVEE_DVD_ERROR_READ_FAILED;

   status = read_file_bytes(disc, &entry, 0, vob_data, entry.size);
   if (status == DEEVEE_DVD_OK)
      status = walk_video_payloads_in_buffer(vob_data, entry.size,
            callback, user_data);

   free(vob_data);
   return status;
}

enum deevee_dvd_status deevee_dvd_walk_vts_menu_pgc_video_payloads(
      struct deevee_disc *disc, unsigned vts_number, unsigned pgc_index,
      deevee_dvd_video_payload_callback callback, void *user_data)
{
   char ifo_path[32];
   char vob_path[32];
   enum deevee_iso_status iso_status;
   struct deevee_iso_entry ifo_entry;
   struct deevee_iso_entry vob_entry;
   uint8_t ifo_start[DEEVEE_DVD_IFO_PROBE_BYTES];
   uint8_t table_header[16];
   uint8_t pgc_header[0xec];
   uint8_t cell_playback[24];
   uint32_t vtsm_pgci_ut;
   uint32_t table_byte_offset;
   uint32_t language_start_byte;
   uint32_t language_byte_offset;
   uint16_t pgc_count;
   uint32_t pgc_start_byte;
   uint32_t pgc_byte_offset;
   uint16_t cell_playback_table_offset;
   uint8_t cell_count;
   uint32_t first_cell_start_sector;
   uint32_t last_cell_end_sector;
   uint32_t vob_sector_count;
   uint32_t span_sector_count;
   uint32_t span_byte_offset;
   uint32_t span_size;
   uint8_t *span_data;
   enum deevee_dvd_status status;

   if (!disc || !callback || !vts_number || vts_number > 99)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   snprintf(ifo_path, sizeof(ifo_path), "/VIDEO_TS/VTS_%02u_0.IFO",
         vts_number);
   snprintf(vob_path, sizeof(vob_path), "/VIDEO_TS/VTS_%02u_0.VOB",
         vts_number);

   iso_status = deevee_iso_find_path(disc, ifo_path, &ifo_entry);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_IFO_NOT_FOUND;
   iso_status = deevee_iso_find_path(disc, vob_path, &vob_entry);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   if (ifo_entry.is_directory || vob_entry.is_directory ||
         !ifo_entry.size || !vob_entry.size)
      return DEEVEE_DVD_ERROR_MALFORMED;

   status = read_file_start(disc, &ifo_entry, ifo_start, sizeof(ifo_start));
   if (status != DEEVEE_DVD_OK)
      return status;

   if (memcmp(ifo_start, DEEVEE_DVD_VTS_IDENTIFIER, 12) != 0)
      return DEEVEE_DVD_ERROR_MALFORMED;

   vtsm_pgci_ut = read_be32(ifo_start + 0xd0);
   if (!vtsm_pgci_ut)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   table_byte_offset = vtsm_pgci_ut * DEEVEE_DVD_SECTOR_SIZE;
   status = read_file_bytes(disc, &ifo_entry, table_byte_offset,
         table_header, sizeof(table_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   if (!read_be16(table_header))
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   language_start_byte = read_be32(table_header + 12);
   language_byte_offset = table_byte_offset + language_start_byte;
   status = read_file_bytes(disc, &ifo_entry, language_byte_offset,
         table_header, sizeof(table_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   pgc_count = read_be16(table_header);
   if (!pgc_count || pgc_index >= pgc_count)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   status = read_file_bytes(disc, &ifo_entry,
         language_byte_offset + 8u + pgc_index * 8u,
         table_header, 8);
   if (status != DEEVEE_DVD_OK)
      return status;

   pgc_start_byte = read_be32(table_header + 4);
   pgc_byte_offset = language_byte_offset + pgc_start_byte;
   status = read_file_bytes(disc, &ifo_entry, pgc_byte_offset,
         pgc_header, sizeof(pgc_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   cell_count = pgc_header[3];
   cell_playback_table_offset = read_be16(pgc_header + 0xe8);
   if (!cell_count || !cell_playback_table_offset)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   status = read_file_bytes(disc, &ifo_entry,
         pgc_byte_offset + cell_playback_table_offset,
         cell_playback, sizeof(cell_playback));
   if (status != DEEVEE_DVD_OK)
      return status;

   first_cell_start_sector = read_be32(cell_playback + 8);
   status = read_file_bytes(disc, &ifo_entry,
         pgc_byte_offset + cell_playback_table_offset +
            (uint32_t)(cell_count - 1u) * 24u,
         cell_playback, sizeof(cell_playback));
   if (status != DEEVEE_DVD_OK)
      return status;

   last_cell_end_sector = read_be32(cell_playback + 20);
   vob_sector_count = (vob_entry.size + DEEVEE_DVD_SECTOR_SIZE - 1) /
      DEEVEE_DVD_SECTOR_SIZE;
   if (first_cell_start_sector > last_cell_end_sector ||
         last_cell_end_sector >= vob_sector_count)
      return DEEVEE_DVD_ERROR_MALFORMED;

   span_sector_count = last_cell_end_sector - first_cell_start_sector + 1u;
   span_byte_offset = first_cell_start_sector * DEEVEE_DVD_SECTOR_SIZE;
   span_size = span_sector_count * DEEVEE_DVD_SECTOR_SIZE;
   if ((uint64_t)span_byte_offset + span_size > vob_entry.size)
      span_size = vob_entry.size - span_byte_offset;

   span_data = (uint8_t *)malloc(span_size);
   if (!span_data)
      return DEEVEE_DVD_ERROR_READ_FAILED;

   status = read_file_bytes(disc, &vob_entry, span_byte_offset,
         span_data, span_size);
   if (status == DEEVEE_DVD_OK)
      status = walk_video_payloads_in_buffer(span_data, span_size,
            callback, user_data);

   free(span_data);
   return status;
}

static void probe_menu_render_streams_in_buffer(const uint8_t *data,
      size_t data_size, struct deevee_dvd_menu_render_probe *probe)
{
   size_t offset;

   if (!data || !probe)
      return;

   probe->scanned_bytes += (uint32_t)data_size;

   for (offset = 0; offset + 9 <= data_size; offset++)
   {
      uint8_t stream_id;
      size_t payload_offset;
      size_t payload_size;
      size_t packet_end;

      if (!is_start_code(data + offset))
         continue;

      stream_id = data[offset + 3];
      if (stream_id >= 0xe0 && stream_id <= 0xef)
      {
         probe->video_pes_packets++;
         probe->has_video = true;
      }
      else if (stream_id == 0xbd &&
            read_pes_payload_bounds_from_buffer(data, data_size, offset,
               &payload_offset, &payload_size, &packet_end))
      {
         uint8_t substream_id;

         probe->private_stream_1_packets++;
         if (!payload_size)
            continue;

         substream_id = data[payload_offset];
         if (substream_id >= 0x20 && substream_id <= 0x3f)
         {
            probe->subpicture_packets++;
            probe->has_subpicture = true;
            if (!probe->first_subpicture_stream_id)
               probe->first_subpicture_stream_id = substream_id;
         }
      }
      else if (stream_id == 0xbf && offset + 7 <= data_size)
      {
         uint8_t substream_id = data[offset + 6];

         probe->private_stream_2_packets++;
         probe->has_nav = true;
         if (substream_id == 0x00)
         {
            const uint8_t *pci = data + offset + 7;
            size_t pci_size = data_size - (offset + 7);

            probe->nav_pci_packets++;
            if (pci_size >= 0x8e)
            {
               uint8_t button_count = pci[0x71];
               uint8_t first_button = pci[0x70];
               uint8_t forced_select = pci[0x74];
               uint8_t forced_action = pci[0x75];
               uint8_t i;

               if (button_count > DEEVEE_DVD_MAX_MENU_BUTTONS)
                  button_count = DEEVEE_DVD_MAX_MENU_BUTTONS;
               if (0x8e + (size_t)button_count * 18u <= pci_size)
               {
                  probe->starting_button = first_button;
                  probe->forced_select_button = forced_select;
                  probe->forced_action_button = forced_action;
                  probe->button_count = button_count;

                  for (i = 0; i < button_count; i++)
                  {
                     const uint8_t *entry = pci + 0x8e + (size_t)i * 18u;
                     struct deevee_dvd_menu_button *button =
                        &probe->buttons[i];

                     button->number = (uint8_t)(i + 1u);
                     button->color_table = entry[0] >> 6;
                     button->x_start = (uint16_t)(((entry[0] & 0x3fu) << 4) |
                           (entry[1] >> 4));
                     button->x_end = (uint16_t)(((entry[1] & 0x03u) << 8) |
                           entry[2]);
                     button->auto_action = (entry[3] & 0x40u) != 0;
                     button->y_start = (uint16_t)(((entry[3] & 0x3fu) << 4) |
                           (entry[4] >> 4));
                     button->y_end = (uint16_t)(((entry[4] & 0x03u) << 8) |
                           entry[5]);
                     button->up = entry[6] & 0x3fu;
                     button->down = entry[7] & 0x3fu;
                     button->left = entry[8] & 0x3fu;
                     button->right = entry[9] & 0x3fu;
                     memcpy(button->command, entry + 10,
                           sizeof(button->command));
                  }
               }
            }
         }
         else if (substream_id == 0x01)
            probe->nav_dsi_packets++;
      }
   }
}

enum deevee_dvd_status deevee_dvd_probe_vts_menu_pgc_render_streams(
      struct deevee_disc *disc, unsigned vts_number, unsigned pgc_index,
      struct deevee_dvd_menu_render_probe *probe)
{
   char ifo_path[32];
   char vob_path[32];
   enum deevee_iso_status iso_status;
   struct deevee_iso_entry ifo_entry;
   struct deevee_iso_entry vob_entry;
   uint8_t ifo_start[DEEVEE_DVD_IFO_PROBE_BYTES];
   uint8_t table_header[16];
   uint8_t pgc_header[0xec];
   uint8_t cell_playback[24];
   uint32_t vtsm_pgci_ut;
   uint32_t table_byte_offset;
   uint32_t language_start_byte;
   uint32_t language_byte_offset;
   uint16_t pgc_count;
   uint32_t pgc_start_byte;
   uint32_t pgc_byte_offset;
   uint16_t command_table_offset;
   uint16_t cell_playback_table_offset;
   uint8_t command_table[8];
   uint8_t cell_count;
   uint32_t first_cell_start_sector;
   uint32_t last_cell_end_sector;
   uint32_t vob_sector_count;
   uint32_t span_sector_count;
   uint32_t span_byte_offset;
   uint32_t span_size;
   uint8_t *span_data;
   enum deevee_dvd_status status;

   if (!disc || !probe || !vts_number || vts_number > 99)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   memset(probe, 0, sizeof(*probe));

   snprintf(ifo_path, sizeof(ifo_path), "/VIDEO_TS/VTS_%02u_0.IFO",
         vts_number);
   snprintf(vob_path, sizeof(vob_path), "/VIDEO_TS/VTS_%02u_0.VOB",
         vts_number);

   iso_status = deevee_iso_find_path(disc, ifo_path, &ifo_entry);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_IFO_NOT_FOUND;
   iso_status = deevee_iso_find_path(disc, vob_path, &vob_entry);
   if (iso_status != DEEVEE_ISO_OK)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   status = read_file_start(disc, &ifo_entry, ifo_start, sizeof(ifo_start));
   if (status != DEEVEE_DVD_OK)
      return status;
   if (memcmp(ifo_start, DEEVEE_DVD_VTS_IDENTIFIER, 12) != 0)
      return DEEVEE_DVD_ERROR_MALFORMED;

   vtsm_pgci_ut = read_be32(ifo_start + 0xd0);
   if (!vtsm_pgci_ut)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   table_byte_offset = vtsm_pgci_ut * DEEVEE_DVD_SECTOR_SIZE;
   status = read_file_bytes(disc, &ifo_entry, table_byte_offset,
         table_header, sizeof(table_header));
   if (status != DEEVEE_DVD_OK)
      return status;
   if (!read_be16(table_header))
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   language_start_byte = read_be32(table_header + 12);
   language_byte_offset = table_byte_offset + language_start_byte;
   status = read_file_bytes(disc, &ifo_entry, language_byte_offset,
         table_header, sizeof(table_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   pgc_count = read_be16(table_header);
   if (!pgc_count || pgc_index >= pgc_count)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   status = read_file_bytes(disc, &ifo_entry,
         language_byte_offset + 8u + pgc_index * 8u, table_header, 8);
   if (status != DEEVEE_DVD_OK)
      return status;

   pgc_start_byte = read_be32(table_header + 4);
   pgc_byte_offset = language_byte_offset + pgc_start_byte;
   status = read_file_bytes(disc, &ifo_entry, pgc_byte_offset,
         pgc_header, sizeof(pgc_header));
   if (status != DEEVEE_DVD_OK)
      return status;

   cell_count = pgc_header[3];
   command_table_offset = read_be16(pgc_header + 0xe4);
   cell_playback_table_offset = read_be16(pgc_header + 0xe8);
   if (!cell_count || !cell_playback_table_offset)
      return DEEVEE_DVD_ERROR_TABLE_NOT_FOUND;

   if (command_table_offset &&
         read_file_bytes(disc, &ifo_entry, pgc_byte_offset +
            command_table_offset, command_table,
            sizeof(command_table)) == DEEVEE_DVD_OK)
   {
      probe->pre_command_count = read_be16(command_table);
      probe->post_command_count = read_be16(command_table + 2);
      probe->cell_command_count = read_be16(command_table + 4);
      if (probe->post_command_count)
      {
         uint16_t command_index;
         uint16_t commands_to_read = probe->post_command_count;

         if (commands_to_read > DEEVEE_DVD_MAX_PROBED_COMMANDS)
            commands_to_read = DEEVEE_DVD_MAX_PROBED_COMMANDS;

         for (command_index = 0; command_index < commands_to_read;
               command_index++)
         {
            if (read_file_bytes(disc, &ifo_entry, pgc_byte_offset +
                  command_table_offset + 8u +
                  ((uint32_t)probe->pre_command_count + command_index) * 8u,
                  probe->post_commands[command_index],
                  sizeof(probe->post_commands[command_index])) !=
                  DEEVEE_DVD_OK)
               break;
            probe->parsed_post_command_count++;
         }
      }
   }

   status = read_file_bytes(disc, &ifo_entry,
         pgc_byte_offset + cell_playback_table_offset,
         cell_playback, sizeof(cell_playback));
   if (status != DEEVEE_DVD_OK)
      return status;

   first_cell_start_sector = read_be32(cell_playback + 8);
   status = read_file_bytes(disc, &ifo_entry,
         pgc_byte_offset + cell_playback_table_offset +
            (uint32_t)(cell_count - 1u) * 24u,
         cell_playback, sizeof(cell_playback));
   if (status != DEEVEE_DVD_OK)
      return status;
   last_cell_end_sector = read_be32(cell_playback + 20);

   vob_sector_count = (vob_entry.size + DEEVEE_DVD_SECTOR_SIZE - 1) /
      DEEVEE_DVD_SECTOR_SIZE;
   if (first_cell_start_sector > last_cell_end_sector ||
         last_cell_end_sector >= vob_sector_count)
      return DEEVEE_DVD_ERROR_MALFORMED;

   span_sector_count = last_cell_end_sector - first_cell_start_sector + 1u;
   span_byte_offset = first_cell_start_sector * DEEVEE_DVD_SECTOR_SIZE;
   span_size = span_sector_count * DEEVEE_DVD_SECTOR_SIZE;
   if ((uint64_t)span_byte_offset + span_size > vob_entry.size)
      span_size = vob_entry.size - span_byte_offset;

   span_data = (uint8_t *)malloc(span_size);
   if (!span_data)
      return DEEVEE_DVD_ERROR_READ_FAILED;

   status = read_file_bytes(disc, &vob_entry, span_byte_offset,
         span_data, span_size);
   if (status == DEEVEE_DVD_OK)
      probe_menu_render_streams_in_buffer(span_data, span_size, probe);

   free(span_data);
   return status;
}

const char *deevee_dvd_status_name(enum deevee_dvd_status status)
{
   switch (status)
   {
      case DEEVEE_DVD_OK:
         return "ok";
      case DEEVEE_DVD_ERROR_INVALID_ARGUMENT:
         return "invalid_argument";
      case DEEVEE_DVD_ERROR_IFO_NOT_FOUND:
         return "ifo_not_found";
      case DEEVEE_DVD_ERROR_READ_FAILED:
         return "read_failed";
      case DEEVEE_DVD_ERROR_INVALID_VMG:
         return "invalid_vmg";
      case DEEVEE_DVD_ERROR_TABLE_NOT_FOUND:
         return "table_not_found";
      case DEEVEE_DVD_ERROR_MALFORMED:
         return "malformed";
      default:
         return "unknown";
   }
}
