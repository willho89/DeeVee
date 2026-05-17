#include "deevee_content.h"
#include "deevee_disc.h"
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

int main(void)
{
   int ok = 1;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   struct deevee_iso_entry entry;
   struct deevee_content_info content;
   struct deevee_disc disc;

   cleanup_fixture();
   ok = ok && mkdir_one("tests/tmp_disc_probe") == 0;
   ok = ok && write_iso_fixture("tests/tmp_disc_probe/sample.iso", 24);
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
   ok = ok && disc.sector_count == 24;
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
   ok = ok && expect_status(deevee_disc_read_sector(&disc, 24,
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
