#include "deevee_dvd.h"

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
