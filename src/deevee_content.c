#include "deevee_content.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define deevee_stat_type struct _stati64
#define deevee_stat _stati64
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFREG) != 0)
#endif
#else
#define deevee_stat_type struct stat
#define deevee_stat stat
#endif

static bool deevee_stat_path(const char *path, deevee_stat_type *st)
{
   return path && path[0] && deevee_stat(path, st) == 0;
}

static bool deevee_is_regular_file(const char *path)
{
   deevee_stat_type st;
   return deevee_stat_path(path, &st) && S_ISREG(st.st_mode);
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

bool deevee_content_probe(const char *path, struct deevee_content_info *info)
{
   enum deevee_content_type type = DEEVEE_CONTENT_NONE;

   if (!path || !path[0] || !info)
      return false;

   if (deevee_is_regular_file(path))
   {
      if (deevee_has_extension(path, "iso"))
         type = DEEVEE_CONTENT_ISO;
      else if (deevee_has_extension(path, "chd"))
         type = DEEVEE_CONTENT_CHD;
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
      case DEEVEE_CONTENT_NONE:
      default:
         return "none";
   }
}
