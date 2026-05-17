#ifndef DEEVEE_DVD_H
#define DEEVEE_DVD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deevee_disc.h"
#include "deevee_iso.h"

#define DEEVEE_DVD_VMG_IDENTIFIER "DVDVIDEO-VMG"
#define DEEVEE_DVD_IFO_PROBE_BYTES 2048
#define DEEVEE_DVD_MAX_TITLES 99

enum deevee_dvd_status
{
   DEEVEE_DVD_OK = 0,
   DEEVEE_DVD_ERROR_INVALID_ARGUMENT,
   DEEVEE_DVD_ERROR_IFO_NOT_FOUND,
   DEEVEE_DVD_ERROR_READ_FAILED,
   DEEVEE_DVD_ERROR_INVALID_VMG,
   DEEVEE_DVD_ERROR_TABLE_NOT_FOUND,
   DEEVEE_DVD_ERROR_MALFORMED
};

struct deevee_dvd_info
{
   struct deevee_iso_entry video_ts_ifo;
   char vmg_identifier[13];
   bool is_dvd_video;
   uint32_t vmg_last_sector;
   uint32_t vmgi_last_sector;
   uint16_t vmg_title_set_count;
   uint32_t vmgi_last_byte;
   uint32_t first_play_pgc;
   uint32_t vmgm_vobs;
   uint32_t tt_srpt;
   uint32_t vmgm_pgci_ut;
   uint32_t ptl_mait;
   uint32_t vts_atrt;
   uint32_t txtdt_mgi;
   uint32_t vmgm_c_adt;
   uint32_t vmgm_vobu_admap;
};

struct deevee_dvd_title
{
   uint8_t title_type;
   uint8_t angle_count;
   uint16_t chapter_count;
   uint16_t parental_management_mask;
   uint8_t vts_number;
   uint8_t vts_title_number;
   uint32_t vts_start_sector;
};

struct deevee_dvd_title_table
{
   uint16_t title_count;
   uint32_t last_byte;
   uint16_t parsed_title_count;
   struct deevee_dvd_title titles[DEEVEE_DVD_MAX_TITLES];
};

enum deevee_dvd_status deevee_dvd_probe(struct deevee_disc *disc,
      struct deevee_dvd_info *info);
enum deevee_dvd_status deevee_dvd_read_title_table(struct deevee_disc *disc,
      const struct deevee_dvd_info *info,
      struct deevee_dvd_title_table *table);
const char *deevee_dvd_status_name(enum deevee_dvd_status status);

#endif
