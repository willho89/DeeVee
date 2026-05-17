#include "deevee_disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#ifndef HAVE_CHD
#define HAVE_CHD 0
#endif

#if HAVE_CHD
#if defined(__has_include)
#if __has_include(<libchdr/chd.h>)
#include <libchdr/chd.h>
#elif __has_include(<chd.h>)
#include <chd.h>
#else
#error "HAVE_CHD=1 requires libchdr/chd.h or chd.h"
#endif
#else
#include <libchdr/chd.h>
#endif
#endif

#ifdef _WIN32
#define deevee_fseek _fseeki64
#define deevee_ftell _ftelli64
typedef __int64 deevee_file_offset;
#else
#define deevee_fseek fseeko
#define deevee_ftell ftello
typedef off_t deevee_file_offset;
#endif

static enum deevee_disc_status deevee_disc_open_iso(struct deevee_disc *disc,
      const char *path)
{
   FILE *file;
   deevee_file_offset size;

   file = fopen(path, "rb");
   if (!file)
      return DEEVEE_DISC_ERROR_OPEN_FAILED;

   if (deevee_fseek(file, 0, SEEK_END) != 0)
   {
      fclose(file);
      return DEEVEE_DISC_ERROR_SEEK_FAILED;
   }

   size = deevee_ftell(file);
   if (size < 0)
   {
      fclose(file);
      return DEEVEE_DISC_ERROR_SEEK_FAILED;
   }

   if (deevee_fseek(file, 0, SEEK_SET) != 0)
   {
      fclose(file);
      return DEEVEE_DISC_ERROR_SEEK_FAILED;
   }

   disc->handle = file;
   disc->type = DEEVEE_CONTENT_ISO;
   disc->sector_count = (uint64_t)size / DEEVEE_DVD_SECTOR_SIZE;
   return DEEVEE_DISC_OK;
}

#if HAVE_CHD
static enum deevee_disc_status deevee_disc_open_chd(struct deevee_disc *disc,
      const char *path)
{
   chd_file *chd = NULL;
   const chd_header *header;
   uint32_t unit_bytes;
   uint32_t data_offset;
   chd_error error;

   error = chd_open(path, CHD_OPEN_READ, NULL, &chd);
   if (error != CHDERR_NONE)
      return DEEVEE_DISC_ERROR_OPEN_FAILED;

   header = chd_get_header(chd);
   if (!header || !header->hunkbytes)
   {
      chd_close(chd);
      return DEEVEE_DISC_ERROR_UNSUPPORTED;
   }

   unit_bytes = header->unitbytes ? header->unitbytes : header->hunkbytes;
   data_offset = 0;

   if (unit_bytes == DEEVEE_DVD_SECTOR_SIZE)
      data_offset = 0;
   else if (unit_bytes == 2448)
      data_offset = 0;
   else
   {
      chd_close(chd);
      return DEEVEE_DISC_ERROR_UNSUPPORTED;
   }

   if (header->hunkbytes % unit_bytes != 0 ||
         unit_bytes < DEEVEE_DVD_SECTOR_SIZE ||
         data_offset > unit_bytes - DEEVEE_DVD_SECTOR_SIZE)
   {
      chd_close(chd);
      return DEEVEE_DISC_ERROR_UNSUPPORTED;
   }

   disc->chd_hunk_cache = (uint8_t *)malloc(header->hunkbytes);
   if (!disc->chd_hunk_cache)
   {
      chd_close(chd);
      return DEEVEE_DISC_ERROR_OPEN_FAILED;
   }

   disc->handle = chd;
   disc->type = DEEVEE_CONTENT_CHD;
   disc->sector_count = header->unitcount ? header->unitcount :
      (uint64_t)header->logicalbytes / unit_bytes;
   disc->chd_hunk_bytes = header->hunkbytes;
   disc->chd_unit_bytes = unit_bytes;
   disc->chd_unit_data_offset = data_offset;
   disc->chd_sectors_per_hunk = header->hunkbytes / unit_bytes;
   disc->chd_cached_hunk = 0;
   disc->chd_cache_valid = false;
   return DEEVEE_DISC_OK;
}
#endif

void deevee_disc_init(struct deevee_disc *disc)
{
   if (!disc)
      return;

   memset(disc, 0, sizeof(*disc));
   disc->type = DEEVEE_CONTENT_NONE;
}

void deevee_disc_close(struct deevee_disc *disc)
{
   if (!disc)
      return;

   if (disc->handle && disc->type == DEEVEE_CONTENT_ISO)
      fclose((FILE *)disc->handle);
#if HAVE_CHD
   else if (disc->handle && disc->type == DEEVEE_CONTENT_CHD)
      chd_close((chd_file *)disc->handle);
#endif
   free(disc->chd_hunk_cache);

   deevee_disc_init(disc);
}

enum deevee_disc_status deevee_disc_open(struct deevee_disc *disc,
      const struct deevee_content_info *content)
{
   if (!disc || !content)
      return DEEVEE_DISC_ERROR_INVALID_ARGUMENT;

   deevee_disc_close(disc);

   switch (content->type)
   {
      case DEEVEE_CONTENT_ISO:
         return deevee_disc_open_iso(disc, content->path);
      case DEEVEE_CONTENT_CHD:
#if HAVE_CHD
         return deevee_disc_open_chd(disc, content->path);
#else
         return DEEVEE_DISC_ERROR_UNSUPPORTED;
#endif
      case DEEVEE_CONTENT_NONE:
      default:
         return DEEVEE_DISC_ERROR_INVALID_ARGUMENT;
   }
}

enum deevee_disc_status deevee_disc_read_sector(struct deevee_disc *disc,
      uint64_t lba, uint8_t *out, size_t out_size)
{
   FILE *file;
   uint64_t offset;

   if (!disc || !disc->handle || !out || out_size < DEEVEE_DVD_SECTOR_SIZE)
      return DEEVEE_DISC_ERROR_INVALID_ARGUMENT;

   if (lba >= disc->sector_count)
      return DEEVEE_DISC_ERROR_SEEK_FAILED;

   if (disc->type == DEEVEE_CONTENT_CHD)
   {
#if HAVE_CHD
      uint32_t hunk;
      uint32_t sector_in_hunk;
      chd_error error;

      if (!disc->chd_hunk_cache || !disc->chd_sectors_per_hunk)
         return DEEVEE_DISC_ERROR_INVALID_ARGUMENT;

      hunk = (uint32_t)(lba / disc->chd_sectors_per_hunk);
      sector_in_hunk = (uint32_t)(lba % disc->chd_sectors_per_hunk);
      if (!disc->chd_cache_valid || disc->chd_cached_hunk != hunk)
      {
         error = chd_read((chd_file *)disc->handle, hunk,
               disc->chd_hunk_cache);
         if (error != CHDERR_NONE)
            return DEEVEE_DISC_ERROR_READ_FAILED;

         disc->chd_cached_hunk = hunk;
         disc->chd_cache_valid = true;
      }

      memcpy(out, disc->chd_hunk_cache +
            (size_t)sector_in_hunk * disc->chd_unit_bytes +
            disc->chd_unit_data_offset,
            DEEVEE_DVD_SECTOR_SIZE);
      return DEEVEE_DISC_OK;
#else
      return DEEVEE_DISC_ERROR_UNSUPPORTED;
#endif
   }

   if (disc->type != DEEVEE_CONTENT_ISO)
      return DEEVEE_DISC_ERROR_UNSUPPORTED;

   offset = lba * DEEVEE_DVD_SECTOR_SIZE;
   file = (FILE *)disc->handle;

   if (deevee_fseek(file, (deevee_file_offset)offset, SEEK_SET) != 0)
      return DEEVEE_DISC_ERROR_SEEK_FAILED;

   if (fread(out, 1, DEEVEE_DVD_SECTOR_SIZE, file) != DEEVEE_DVD_SECTOR_SIZE)
      return DEEVEE_DISC_ERROR_READ_FAILED;

   return DEEVEE_DISC_OK;
}

const char *deevee_disc_status_name(enum deevee_disc_status status)
{
   switch (status)
   {
      case DEEVEE_DISC_OK:
         return "ok";
      case DEEVEE_DISC_ERROR_INVALID_ARGUMENT:
         return "invalid_argument";
      case DEEVEE_DISC_ERROR_UNSUPPORTED:
         return "unsupported";
      case DEEVEE_DISC_ERROR_OPEN_FAILED:
         return "open_failed";
      case DEEVEE_DISC_ERROR_SEEK_FAILED:
         return "seek_failed";
      case DEEVEE_DISC_ERROR_READ_FAILED:
         return "read_failed";
      default:
         return "unknown";
   }
}
