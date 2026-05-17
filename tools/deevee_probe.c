#include "deevee_content.h"
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
         enum deevee_dvd_status title_status;
         enum deevee_dvd_status menu_status;
         enum deevee_dvd_status menu_pgc_status;
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
