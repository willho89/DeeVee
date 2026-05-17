#include "deevee_content.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define stat _stat
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFREG) != 0)
#endif
#endif

static bool deevee_stat_path(const char *path, struct stat *st)
{
   return path && path[0] && stat(path, st) == 0;
}

static bool deevee_is_directory(const char *path)
{
   struct stat st;
   return deevee_stat_path(path, &st) && S_ISDIR(st.st_mode);
}

static bool deevee_is_regular_file(const char *path)
{
   struct stat st;
   return deevee_stat_path(path, &st) && S_ISREG(st.st_mode);
}

static bool deevee_join_path(char *out, size_t out_size,
      const char *base, const char *leaf)
{
   int written;
   size_t base_len;
   const char *separator = "/";

   if (!out || !out_size || !base || !leaf)
      return false;

   base_len = strlen(base);
   if (base_len && (base[base_len - 1] == '/' || base[base_len - 1] == '\\'))
      separator = "";

   written = snprintf(out, out_size, "%s%s%s", base, separator, leaf);
   return written > 0 && (size_t)written < out_size;
}

static bool deevee_has_extension(const char *path, const char *extension)
{
   const char *dot;

   if (!path || !extension)
      return false;

   dot = strrchr(path, '.');
   if (!dot)
      return false;

   dot++;
   while (*dot && *extension)
   {
      if (tolower((unsigned char)*dot) !=
            tolower((unsigned char)*extension))
         return false;
      dot++;
      extension++;
   }

   return *dot == '\0' && *extension == '\0';
}

static bool deevee_folder_has_video_ts(const char *path)
{
   char nested_ifo[DEEVEE_MAX_PATH];
   char direct_ifo[DEEVEE_MAX_PATH];

   if (!deevee_join_path(nested_ifo, sizeof(nested_ifo),
            path, "VIDEO_TS/VIDEO_TS.IFO"))
      return false;

   if (deevee_is_regular_file(nested_ifo))
      return true;

   if (!deevee_join_path(direct_ifo, sizeof(direct_ifo),
            path, "VIDEO_TS.IFO"))
      return false;

   return deevee_is_regular_file(direct_ifo);
}

bool deevee_content_probe(const char *path, struct deevee_content_info *info)
{
   enum deevee_content_type type = DEEVEE_CONTENT_NONE;

   if (!path || !path[0] || !info)
      return false;

   if (deevee_is_directory(path))
   {
      if (!deevee_folder_has_video_ts(path))
         return false;
      type = DEEVEE_CONTENT_DVD_FOLDER;
   }
   else if (deevee_is_regular_file(path))
   {
      if (deevee_has_extension(path, "iso"))
         type = DEEVEE_CONTENT_ISO;
      else if (deevee_has_extension(path, "chd"))
         type = DEEVEE_CONTENT_CHD;
      else if (deevee_has_extension(path, "ifo"))
         type = DEEVEE_CONTENT_IFO;
      else
         return false;
   }
   else
      return false;

   memset(info, 0, sizeof(*info));
   info->type = type;
   snprintf(info->path, sizeof(info->path), "%s", path);
   return true;
}

const char *deevee_content_type_name(enum deevee_content_type type)
{
   switch (type)
   {
      case DEEVEE_CONTENT_ISO:
         return "DVD ISO";
      case DEEVEE_CONTENT_CHD:
         return "DVD CHD";
      case DEEVEE_CONTENT_IFO:
         return "DVD IFO";
      case DEEVEE_CONTENT_DVD_FOLDER:
         return "DVD folder";
      case DEEVEE_CONTENT_NONE:
      default:
         return "none";
   }
}
