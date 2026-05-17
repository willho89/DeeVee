#include "deevee_disc.h"

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
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
         return DEEVEE_DISC_ERROR_UNSUPPORTED;
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

   if (disc->type != DEEVEE_CONTENT_ISO)
      return DEEVEE_DISC_ERROR_UNSUPPORTED;

   if (lba >= disc->sector_count)
      return DEEVEE_DISC_ERROR_SEEK_FAILED;

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
