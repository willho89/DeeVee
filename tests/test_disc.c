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
         write_record(sector, offset, 22, 1234, 0,
               (const uint8_t *)"VIDEO_TS.IFO;1", 14);
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
   struct deevee_dvd_title_table title_table;
   struct deevee_iso_entry entry;
   struct deevee_content_info content;
   struct deevee_disc disc;

   cleanup_fixture();
   ok = ok && mkdir_one("tests/tmp_disc_probe") == 0;
   ok = ok && write_iso_fixture("tests/tmp_disc_probe/sample.iso", 40);
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
   ok = ok && disc.sector_count == 40;
   ok = ok && expect_status(deevee_disc_read_sector(&disc, 16,
            sector, sizeof(sector)), DEEVEE_DISC_OK, "read sector 16");
   ok = ok && memcmp(sector + 1, "CD001", 5) == 0;
   ok = ok && expect_iso_status(deevee_iso_find_path(&disc,
            "/VIDEO_TS/VIDEO_TS.IFO", &entry), DEEVEE_ISO_OK,
         "find VIDEO_TS.IFO");
   ok = ok && entry.lba == 22 && entry.size == 1234 && !entry.is_directory;
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
   ok = ok && expect_status(deevee_disc_read_sector(&disc, 40,
            sector, sizeof(sector)), DEEVEE_DISC_ERROR_SEEK_FAILED,
         "read beyond end");
   deevee_disc_close(&disc);

   ok = ok && deevee_content_probe("tests/tmp_disc_probe/sample.chd", &content);
   ok = ok && expect_status(deevee_disc_open(&disc, &content),
         DEEVEE_DISC_ERROR_UNSUPPORTED, "open chd");
   deevee_disc_close(&disc);

   cleanup_fixture();
   return ok ? 0 : 1;
}
