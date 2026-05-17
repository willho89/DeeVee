#include "deevee_content.h"

#include <stdio.h>
#include <stdlib.h>
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

static int write_empty_file(const char *path)
{
   FILE *file = fopen(path, "wb");
   if (!file)
      return 0;
   fclose(file);
   return 1;
}

static void cleanup_fixture(void)
{
   remove("tests/tmp_content_probe/movie.iso");
   remove("tests/tmp_content_probe/movie.chd");
   remove("tests/tmp_content_probe/VIDEO_TS.IFO");
   remove("tests/tmp_content_probe/readme.txt");
   remove("tests/tmp_content_probe/disc/VIDEO_TS/VIDEO_TS.IFO");
   rmdir_one("tests/tmp_content_probe/disc/VIDEO_TS");
   rmdir_one("tests/tmp_content_probe/disc");
   rmdir_one("tests/tmp_content_probe");
}

static int expect_type(const char *path, enum deevee_content_type type)
{
   struct deevee_content_info info;
   if (!deevee_content_probe(path, &info))
   {
      fprintf(stderr, "expected probe success for %s\n", path);
      return 0;
   }

   if (info.type != type)
   {
      fprintf(stderr, "unexpected type for %s: %s\n",
            path, deevee_content_type_name(info.type));
      return 0;
   }

   return 1;
}

static int expect_reject(const char *path)
{
   struct deevee_content_info info;
   if (deevee_content_probe(path, &info))
   {
      fprintf(stderr, "expected probe failure for %s\n", path);
      return 0;
   }

   return 1;
}

int main(void)
{
   int ok = 1;

   cleanup_fixture();
   ok = ok && mkdir_one("tests/tmp_content_probe") == 0;
   ok = ok && mkdir_one("tests/tmp_content_probe/disc") == 0;
   ok = ok && mkdir_one("tests/tmp_content_probe/disc/VIDEO_TS") == 0;
   ok = ok && write_empty_file("tests/tmp_content_probe/movie.iso");
   ok = ok && write_empty_file("tests/tmp_content_probe/movie.chd");
   ok = ok && write_empty_file("tests/tmp_content_probe/VIDEO_TS.IFO");
   ok = ok && write_empty_file("tests/tmp_content_probe/readme.txt");
   ok = ok && write_empty_file(
         "tests/tmp_content_probe/disc/VIDEO_TS/VIDEO_TS.IFO");

   if (!ok)
   {
      fprintf(stderr, "failed to create content probe fixture\n");
      cleanup_fixture();
      return 1;
   }

   ok = expect_type("tests/tmp_content_probe/movie.iso",
         DEEVEE_CONTENT_ISO);
   ok = ok && expect_type("tests/tmp_content_probe/movie.chd",
         DEEVEE_CONTENT_CHD);
   ok = ok && expect_reject("tests/tmp_content_probe/VIDEO_TS.IFO");
   ok = ok && expect_reject("tests/tmp_content_probe/disc");
   ok = ok && expect_reject("tests/tmp_content_probe/readme.txt");
   ok = ok && expect_reject("tests/tmp_content_probe/missing.iso");

   cleanup_fixture();
   return ok ? 0 : 1;
}
