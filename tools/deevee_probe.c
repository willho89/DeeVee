#include "deevee_content.h"

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
