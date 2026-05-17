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

static enum deevee_dvd_status read_file_start(struct deevee_disc *disc,
      const struct deevee_iso_entry *entry, uint8_t *out, size_t out_size)
{
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   size_t copied = 0;

   if (!disc || !entry || !out || !out_size)
      return DEEVEE_DVD_ERROR_INVALID_ARGUMENT;

   while (copied < out_size && copied < entry->size)
   {
      uint64_t lba = entry->lba + copied / DEEVEE_DVD_SECTOR_SIZE;
      size_t sector_offset = copied % DEEVEE_DVD_SECTOR_SIZE;
      size_t available = DEEVEE_DVD_SECTOR_SIZE - sector_offset;
      size_t remaining = out_size - copied;
      size_t to_copy = available < remaining ? available : remaining;

      if (deevee_disc_read_sector(disc, lba, sector, sizeof(sector)) !=
            DEEVEE_DISC_OK)
         return DEEVEE_DVD_ERROR_READ_FAILED;

      memcpy(out + copied, sector + sector_offset, to_copy);
      copied += to_copy;
   }

   return copied ? DEEVEE_DVD_OK : DEEVEE_DVD_ERROR_READ_FAILED;
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
