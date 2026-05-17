#include "deevee_content.h"
#include "deevee_disc.h"
#include "deevee_dvd.h"
#include "deevee_iso.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_one(path) _mkdir(path)
#define rmdir_one(path) _rmdir(path)
#else
#include <unistd.h>
#define mkdir_one(path) mkdir(path, 0777)
#define rmdir_one(path) rmdir(path)
#endif

static void cleanup_fixture(void)
{
   remove("tests/tmp_disc_probe/sample.iso");
   remove("tests/tmp_disc_probe/sample.chd");
   rmdir_one("tests/tmp_disc_probe");
}

static void put_le32(uint8_t *data, uint32_t value)
{
   data[0] = (uint8_t)(value & 0xffu);
   data[1] = (uint8_t)((value >> 8) & 0xffu);
   data[2] = (uint8_t)((value >> 16) & 0xffu);
   data[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void put_be16(uint8_t *data, uint16_t value)
{
   data[0] = (uint8_t)((value >> 8) & 0xffu);
   data[1] = (uint8_t)(value & 0xffu);
}

static void put_be32(uint8_t *data, uint32_t value)
{
   data[0] = (uint8_t)((value >> 24) & 0xffu);
   data[1] = (uint8_t)((value >> 16) & 0xffu);
   data[2] = (uint8_t)((value >> 8) & 0xffu);
   data[3] = (uint8_t)(value & 0xffu);
}

static void put_start_code(uint8_t *data, uint8_t code)
{
   data[0] = 0x00;
   data[1] = 0x00;
   data[2] = 0x01;
   data[3] = code;
}

static void put_private_stream_1(uint8_t *data, uint8_t substream_id)
{
   put_start_code(data, 0xbd);
   put_be16(data + 4, 10);
   data[6] = 0x80;
   data[7] = 0x00;
   data[8] = 0;
   data[9] = substream_id;
}

static void put_private_stream_2(uint8_t *data, uint8_t substream_id)
{
   put_start_code(data, 0xbf);
   put_be16(data + 4, 4);
   data[6] = substream_id;
}

static void put_video_pes(uint8_t *data)
{
   put_start_code(data, 0xe0);
   put_be16(data + 4, 20);
   data[6] = 0x80;
   data[7] = 0x00;
   data[8] = 0;
   put_start_code(data + 9, 0xb3);
   data[13] = 0x2d;
   data[14] = 0x02;
   data[15] = 0x40;
   data[16] = 0x33;
   data[17] = 0x04;
   data[18] = 0x00;
   data[19] = 0x00;
   data[20] = 0x20;
}

static size_t write_record(uint8_t *sector, size_t offset, uint32_t lba,
      uint32_t size, uint8_t flags, const uint8_t *name, uint8_t name_len)
{
   size_t length = 33u + name_len + ((name_len & 1u) ? 0u : 1u);
   uint8_t *record = sector + offset;

   memset(record, 0, length);
   record[0] = (uint8_t)length;
   put_le32(record + 2, lba);
   put_le32(record + 10, size);
   record[25] = flags;
   record[28] = 1;
   record[31] = 1;
   record[32] = name_len;
   memcpy(record + 33, name, name_len);
   return offset + length;
}

static void write_title_entry(uint8_t *entry, uint8_t type, uint8_t angles,
      uint16_t chapters, uint16_t parental_mask, uint8_t vts,
      uint8_t vts_title, uint32_t vts_start_sector)
{
   entry[0] = type;
   entry[1] = angles;
   put_be16(entry + 2, chapters);
   put_be16(entry + 4, parental_mask);
   entry[6] = vts;
   entry[7] = vts_title;
   put_be32(entry + 8, vts_start_sector);
}

static void write_pgc_header(uint8_t *pgc)
{
   uint8_t *command_table;
   uint8_t *program_map;
   uint8_t *cell_playback;
   uint8_t *cell_position;

   pgc[2] = 2;
   pgc[3] = 3;
   pgc[4] = 0x00;
   pgc[5] = 0x01;
   pgc[6] = 0x02;
   pgc[7] = 0x03;
   put_be32(pgc + 8, 0x01020304);
   put_be16(pgc + 0xe4, 0x0100);
   put_be16(pgc + 0xe6, 0x0120);
   put_be16(pgc + 0xe8, 0x0140);
   put_be16(pgc + 0xea, 0x0180);

   command_table = pgc + 0x0100;
   put_be16(command_table, 1);
   put_be16(command_table + 2, 2);
   put_be16(command_table + 4, 3);
   put_be16(command_table + 6, 31);

   program_map = pgc + 0x0120;
   program_map[0] = 1;

   cell_playback = pgc + 0x0140;
   put_be32(cell_playback, 0x09080706);
   cell_playback[4] = 0x00;
   cell_playback[5] = 0x00;
   cell_playback[6] = 0x10;
   cell_playback[7] = 0x15;
   put_be32(cell_playback + 8, 2);
   put_be32(cell_playback + 12, 3);
   put_be32(cell_playback + 16, 4);
   put_be32(cell_playback + 20, 5);

   cell_position = pgc + 0x0180;
   put_be16(cell_position, 7);
   cell_position[3] = 9;
}

static int write_sector(FILE *file, const uint8_t *sector)
{
   return fwrite(sector, 1, DEEVEE_DVD_SECTOR_SIZE, file) ==
      DEEVEE_DVD_SECTOR_SIZE;
}

static int write_iso_fixture(const char *path, unsigned sectors)
{
   unsigned i;
   FILE *file = fopen(path, "wb");
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint8_t dot = 0;
   uint8_t dotdot = 1;
   size_t offset;

   if (!file)
      return 0;

   for (i = 0; i < sectors; i++)
   {
      memset(sector, 0, sizeof(sector));

      if (i == 16)
      {
         sector[0] = 1;
         memcpy(sector + 1, "CD001", 5);
         sector[6] = 1;
         write_record(sector, 156, 20, DEEVEE_DVD_SECTOR_SIZE, 2, &dot, 1);
      }
      else if (i == 20)
      {
         offset = 0;
         offset = write_record(sector, offset, 20,
               DEEVEE_DVD_SECTOR_SIZE, 2, &dot, 1);
         offset = write_record(sector, offset, 20,
               DEEVEE_DVD_SECTOR_SIZE, 2, &dotdot, 1);
         write_record(sector, offset, 21, DEEVEE_DVD_SECTOR_SIZE, 2,
               (const uint8_t *)"VIDEO_TS", 8);
      }
      else if (i == 21)
      {
         offset = 0;
         offset = write_record(sector, offset, 21,
               DEEVEE_DVD_SECTOR_SIZE, 2, &dot, 1);
         offset = write_record(sector, offset, 20,
               DEEVEE_DVD_SECTOR_SIZE, 2, &dotdot, 1);
         offset = write_record(sector, offset, 22,
               24 * DEEVEE_DVD_SECTOR_SIZE, 0,
               (const uint8_t *)"VIDEO_TS.IFO;1", 14);
         write_record(sector, offset, 34, 8 * DEEVEE_DVD_SECTOR_SIZE, 0,
               (const uint8_t *)"VIDEO_TS.VOB;1", 14);
      }
      else if (i == 22)
      {
         memcpy(sector, DEEVEE_DVD_VMG_IDENTIFIER, 12);
         put_be32(sector + 0x0c, 99);
         put_be32(sector + 0x1c, 7);
         put_be16(sector + 0x3e, 3);
         put_be32(sector + 0x80, 2047);
         put_be32(sector + 0x84, 8);
         put_be32(sector + 0xc0, 9);
         put_be32(sector + 0xc4, 10);
         put_be32(sector + 0xc8, 11);
         put_be32(sector + 0xcc, 12);
         put_be32(sector + 0xd0, 13);
         put_be32(sector + 0xd4, 14);
         put_be32(sector + 0xd8, 15);
         put_be32(sector + 0xdc, 16);
      }
      else if (i == 32)
      {
         put_be16(sector, 2);
         put_be32(sector + 4, 31);
         write_title_entry(sector + 8, 1, 1, 4, 0, 1, 1, 1000);
         write_title_entry(sector + 20, 2, 2, 8, 0x00ff, 2, 1, 2000);
      }
      else if (i == 33)
      {
         put_be16(sector, 1);
         put_be32(sector + 4, 63);
         sector[8] = 'e';
         sector[9] = 'n';
         sector[10] = 0;
         sector[11] = 0x80;
         put_be32(sector + 12, 40);
         put_be16(sector + 40, 1);
         put_be32(sector + 44, 400);
         put_be32(sector + 48, 0x83000000);
         put_be32(sector + 52, 24);
         write_pgc_header(sector + 64);
      }
      else if (i == 36)
      {
         put_start_code(sector, 0xba);
         put_start_code(sector + 16, 0xbb);
         put_start_code(sector + 32, 0xbc);
         put_private_stream_1(sector + 48, 0x80);
         put_start_code(sector + 64, 0xbe);
         put_private_stream_2(sector + 80, 0x00);
         put_video_pes(sector + 192);
         put_start_code(sector + 112, 0xc0);
         put_start_code(sector + 128, 0xf0);
         put_private_stream_2(sector + 144, 0x01);
         put_private_stream_1(sector + 160, 0xa0);
         put_private_stream_1(sector + 176, 0x20);
      }

      if (!write_sector(file, sector))
      {
         fclose(file);
         return 0;
      }
   }

   fclose(file);
   return 1;
}

static int write_empty_file(const char *path)
{
   FILE *file = fopen(path, "wb");
   if (!file)
      return 0;
   fclose(file);
   return 1;
}

static int expect_status(enum deevee_disc_status actual,
      enum deevee_disc_status expected, const char *context)
{
   if (actual != expected)
   {
      fprintf(stderr, "%s: expected %s, got %s\n",
            context, deevee_disc_status_name(expected),
            deevee_disc_status_name(actual));
      return 0;
   }

   return 1;
}

static int expect_iso_status(enum deevee_iso_status actual,
      enum deevee_iso_status expected, const char *context)
{
   if (actual != expected)
   {
      fprintf(stderr, "%s: expected %s, got %s\n",
            context, deevee_iso_status_name(expected),
            deevee_iso_status_name(actual));
      return 0;
   }

   return 1;
}

static int expect_dvd_status(enum deevee_dvd_status actual,
      enum deevee_dvd_status expected, const char *context)
{
   if (actual != expected)
   {
      fprintf(stderr, "%s: expected %s, got %s\n",
            context, deevee_dvd_status_name(expected),
            deevee_dvd_status_name(actual));
      return 0;
   }

   return 1;
}

int main(void)
{
   int ok = 1;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   struct deevee_dvd_info dvd_info;
   struct deevee_dvd_menu_language_table menu_table;
   struct deevee_dvd_menu_pgc_summary menu_pgc;
   struct deevee_dvd_menu_pgc_table_summary menu_pgc_tables;
   struct deevee_dvd_menu_vob_span menu_vob_span;
   struct deevee_dvd_vob_packet_probe vob_packet_probe;
   struct deevee_dvd_video_probe video_probe;
   struct deevee_dvd_title_table title_table;
   struct deevee_iso_entry entry;
   struct deevee_content_info content;
   struct deevee_disc disc;

   cleanup_fixture();
   ok = ok && mkdir_one("tests/tmp_disc_probe") == 0;
   ok = ok && write_iso_fixture("tests/tmp_disc_probe/sample.iso", 48);
   ok = ok && write_empty_file("tests/tmp_disc_probe/sample.chd");

   if (!ok)
   {
      fprintf(stderr, "failed to create disc probe fixture\n");
      cleanup_fixture();
      return 1;
   }

   deevee_disc_init(&disc);
   ok = ok && deevee_content_probe("tests/tmp_disc_probe/sample.iso", &content);
   ok = ok && expect_status(deevee_disc_open(&disc, &content),
         DEEVEE_DISC_OK, "open iso");
   ok = ok && disc.sector_count == 48;
   ok = ok && expect_status(deevee_disc_read_sector(&disc, 16,
            sector, sizeof(sector)), DEEVEE_DISC_OK, "read sector 16");
   ok = ok && memcmp(sector + 1, "CD001", 5) == 0;
   ok = ok && expect_iso_status(deevee_iso_find_path(&disc,
            "/VIDEO_TS/VIDEO_TS.IFO", &entry), DEEVEE_ISO_OK,
         "find VIDEO_TS.IFO");
   ok = ok && entry.lba == 22 &&
      entry.size == 24 * DEEVEE_DVD_SECTOR_SIZE && !entry.is_directory;
   ok = ok && expect_iso_status(deevee_iso_find_path(&disc,
            "/video_ts/video_ts.ifo", &entry), DEEVEE_ISO_OK,
         "find lowercase VIDEO_TS.IFO");
   ok = ok && expect_iso_status(deevee_iso_find_path(&disc,
            "/VIDEO_TS/MISSING.IFO", &entry), DEEVEE_ISO_ERROR_NOT_FOUND,
         "missing IFO");
   ok = ok && expect_dvd_status(deevee_dvd_probe(&disc, &dvd_info),
         DEEVEE_DVD_OK, "dvd probe");
   ok = ok && dvd_info.is_dvd_video;
   ok = ok && strcmp(dvd_info.vmg_identifier,
         DEEVEE_DVD_VMG_IDENTIFIER) == 0;
   ok = ok && dvd_info.video_ts_ifo.lba == 22;
   ok = ok && dvd_info.vmg_last_sector == 99;
   ok = ok && dvd_info.vmgi_last_sector == 7;
   ok = ok && dvd_info.vmg_title_set_count == 3;
   ok = ok && dvd_info.vmgi_last_byte == 2047;
   ok = ok && dvd_info.first_play_pgc == 8;
   ok = ok && dvd_info.vmgm_vobs == 9;
   ok = ok && dvd_info.tt_srpt == 10;
   ok = ok && dvd_info.vmgm_pgci_ut == 11;
   ok = ok && dvd_info.ptl_mait == 12;
   ok = ok && dvd_info.vts_atrt == 13;
   ok = ok && dvd_info.txtdt_mgi == 14;
   ok = ok && dvd_info.vmgm_c_adt == 15;
   ok = ok && dvd_info.vmgm_vobu_admap == 16;
   ok = ok && expect_dvd_status(deevee_dvd_read_title_table(&disc,
            &dvd_info, &title_table), DEEVEE_DVD_OK, "title table");
   ok = ok && title_table.title_count == 2;
   ok = ok && title_table.parsed_title_count == 2;
   ok = ok && title_table.titles[0].title_type == 1;
   ok = ok && title_table.titles[0].angle_count == 1;
   ok = ok && title_table.titles[0].chapter_count == 4;
   ok = ok && title_table.titles[0].parental_management_mask == 0;
   ok = ok && title_table.titles[0].vts_number == 1;
   ok = ok && title_table.titles[0].vts_title_number == 1;
   ok = ok && title_table.titles[0].vts_start_sector == 1000;
   ok = ok && title_table.titles[1].title_type == 2;
   ok = ok && title_table.titles[1].angle_count == 2;
   ok = ok && title_table.titles[1].chapter_count == 8;
   ok = ok && title_table.titles[1].parental_management_mask == 0x00ff;
   ok = ok && title_table.titles[1].vts_number == 2;
   ok = ok && title_table.titles[1].vts_title_number == 1;
   ok = ok && title_table.titles[1].vts_start_sector == 2000;
   ok = ok && expect_dvd_status(deevee_dvd_read_menu_language_table(&disc,
            &dvd_info, &menu_table), DEEVEE_DVD_OK, "menu language table");
   ok = ok && menu_table.language_count == 1;
   ok = ok && menu_table.last_byte == 63;
   ok = ok && menu_table.parsed_language_count == 1;
   ok = ok && strcmp(menu_table.languages[0].language, "en") == 0;
   ok = ok && menu_table.languages[0].language_extension == 0;
   ok = ok && menu_table.languages[0].menu_existence == 0x80;
   ok = ok && menu_table.languages[0].start_byte == 40;
   ok = ok && expect_dvd_status(deevee_dvd_read_first_menu_pgc(&disc,
            &dvd_info, &menu_pgc), DEEVEE_DVD_OK, "first menu pgc");
   ok = ok && strcmp(menu_pgc.language, "en") == 0;
   ok = ok && menu_pgc.language_unit_start_byte == 40;
   ok = ok && menu_pgc.pgc_count == 1;
   ok = ok && menu_pgc.language_unit_last_byte == 400;
   ok = ok && menu_pgc.pgc_category == 0x83000000;
   ok = ok && menu_pgc.pgc_start_byte == 24;
   ok = ok && menu_pgc.program_count == 2;
   ok = ok && menu_pgc.cell_count == 3;
   ok = ok && menu_pgc.playback_time[0] == 0x00;
   ok = ok && menu_pgc.playback_time[1] == 0x01;
   ok = ok && menu_pgc.playback_time[2] == 0x02;
   ok = ok && menu_pgc.playback_time[3] == 0x03;
   ok = ok && menu_pgc.prohibited_user_ops == 0x01020304;
   ok = ok && menu_pgc.command_table_offset == 0x0100;
   ok = ok && menu_pgc.program_map_offset == 0x0120;
   ok = ok && menu_pgc.cell_playback_table_offset == 0x0140;
   ok = ok && menu_pgc.cell_position_table_offset == 0x0180;
   ok = ok && expect_dvd_status(deevee_dvd_read_first_menu_pgc_tables(&disc,
            &dvd_info, &menu_pgc_tables), DEEVEE_DVD_OK,
         "first menu pgc tables");
   ok = ok && strcmp(menu_pgc_tables.language, "en") == 0;
   ok = ok && menu_pgc_tables.pre_command_count == 1;
   ok = ok && menu_pgc_tables.post_command_count == 2;
   ok = ok && menu_pgc_tables.cell_command_count == 3;
   ok = ok && menu_pgc_tables.command_table_last_byte == 31;
   ok = ok && menu_pgc_tables.first_program_entry_cell == 1;
   ok = ok && menu_pgc_tables.first_cell_category == 0x09080706;
   ok = ok && menu_pgc_tables.first_cell_playback_time[0] == 0x00;
   ok = ok && menu_pgc_tables.first_cell_playback_time[1] == 0x00;
   ok = ok && menu_pgc_tables.first_cell_playback_time[2] == 0x10;
   ok = ok && menu_pgc_tables.first_cell_playback_time[3] == 0x15;
   ok = ok && menu_pgc_tables.first_cell_first_vobu_start_sector == 2;
   ok = ok && menu_pgc_tables.first_cell_first_ilvu_end_sector == 3;
   ok = ok && menu_pgc_tables.first_cell_last_vobu_start_sector == 4;
   ok = ok && menu_pgc_tables.first_cell_last_vobu_end_sector == 5;
   ok = ok && menu_pgc_tables.first_cell_vob_id == 7;
   ok = ok && menu_pgc_tables.first_cell_id == 9;
   ok = ok && expect_dvd_status(deevee_dvd_resolve_first_menu_vob_span(
            &disc, &dvd_info, &menu_vob_span), DEEVEE_DVD_OK,
         "first menu vob span");
   ok = ok && menu_vob_span.vmgm_vob.lba == 34;
   ok = ok && menu_vob_span.vmgm_vob.size == 8 * DEEVEE_DVD_SECTOR_SIZE;
   ok = ok && menu_vob_span.vmgm_vob_sector_count == 8;
   ok = ok && menu_vob_span.first_cell_start_sector == 2;
   ok = ok && menu_vob_span.first_cell_end_sector == 5;
   ok = ok && menu_vob_span.first_cell_start_lba == 36;
   ok = ok && menu_vob_span.first_cell_end_lba == 39;
   ok = ok && menu_vob_span.first_cell_vob_id == 7;
   ok = ok && menu_vob_span.first_cell_id == 9;
   ok = ok && expect_status(deevee_disc_read_sector(&disc,
            menu_vob_span.first_cell_start_lba, sector, sizeof(sector)),
         DEEVEE_DISC_OK, "read first menu cell sector");
   ok = ok && sector[0] == 0x00 && sector[1] == 0x00 &&
      sector[2] == 0x01 && sector[3] == 0xba;
   ok = ok && expect_dvd_status(deevee_dvd_probe_first_menu_vob_packets(
            &disc, &dvd_info, &vob_packet_probe), DEEVEE_DVD_OK,
         "first menu vob packet probe");
   ok = ok && vob_packet_probe.scanned_sectors == 4;
   ok = ok && vob_packet_probe.pack_header_count == 1;
   ok = ok && vob_packet_probe.system_header_count == 1;
   ok = ok && vob_packet_probe.program_stream_map_count == 1;
   ok = ok && vob_packet_probe.private_stream_1_count == 3;
   ok = ok && vob_packet_probe.private_stream_2_count == 2;
   ok = ok && vob_packet_probe.padding_stream_count == 1;
   ok = ok && vob_packet_probe.video_pes_count == 1;
   ok = ok && vob_packet_probe.audio_pes_count == 1;
   ok = ok && vob_packet_probe.ac3_audio_count == 1;
   ok = ok && vob_packet_probe.dts_audio_count == 0;
   ok = ok && vob_packet_probe.lpcm_audio_count == 1;
   ok = ok && vob_packet_probe.subpicture_count == 1;
   ok = ok && vob_packet_probe.private_stream_1_unknown_count == 0;
   ok = ok && vob_packet_probe.nav_pci_count == 1;
   ok = ok && vob_packet_probe.nav_dsi_count == 1;
   ok = ok && vob_packet_probe.private_stream_2_unknown_count == 0;
   ok = ok && vob_packet_probe.other_pes_count == 2;
   ok = ok && vob_packet_probe.nav_pack_count == 2;
   ok = ok && vob_packet_probe.first_video_stream_id == 0xe0;
   ok = ok && vob_packet_probe.first_audio_stream_id == 0x80;
   ok = ok && vob_packet_probe.first_private_stream_1_substream_id == 0x80;
   ok = ok && vob_packet_probe.first_nav_substream_id == 0x00;
   ok = ok && vob_packet_probe.has_pack_header;
   ok = ok && vob_packet_probe.has_video;
   ok = ok && vob_packet_probe.has_audio;
   ok = ok && vob_packet_probe.has_subpicture;
   ok = ok && vob_packet_probe.has_nav;
   ok = ok && expect_dvd_status(deevee_dvd_probe_first_menu_video(&disc,
            &dvd_info, &video_probe), DEEVEE_DVD_OK,
         "first menu video probe");
   ok = ok && video_probe.scanned_sectors == 4;
   ok = ok && video_probe.video_pes_packets == 1;
   ok = ok && video_probe.video_payload_bytes == 17;
   ok = ok && video_probe.first_video_stream_id == 0xe0;
   ok = ok && video_probe.has_video_payload;
   ok = ok && video_probe.has_sequence_header;
   ok = ok && video_probe.sequence_width == 720;
   ok = ok && video_probe.sequence_height == 576;
   ok = ok && video_probe.aspect_ratio_code == 3;
   ok = ok && video_probe.frame_rate_code == 3;
   ok = ok && video_probe.bit_rate_value == 4096;
   ok = ok && video_probe.vbv_buffer_size_value == 4;
   ok = ok && !video_probe.constrained_parameters_flag;
   ok = ok && expect_status(deevee_disc_read_sector(&disc, 48,
            sector, sizeof(sector)), DEEVEE_DISC_ERROR_SEEK_FAILED,
         "read beyond end");
   deevee_disc_close(&disc);

   ok = ok && deevee_content_probe("tests/tmp_disc_probe/sample.chd", &content);
#if HAVE_CHD
   ok = ok && expect_status(deevee_disc_open(&disc, &content),
         DEEVEE_DISC_ERROR_OPEN_FAILED, "open invalid chd");
#else
   ok = ok && expect_status(deevee_disc_open(&disc, &content),
         DEEVEE_DISC_ERROR_UNSUPPORTED, "open chd");
#endif
   deevee_disc_close(&disc);

   cleanup_fixture();
   return ok ? 0 : 1;
}
