#ifndef DEEVEE_CONTENT_H
#define DEEVEE_CONTENT_H

#include <stdbool.h>

#define DEEVEE_MAX_PATH 4096

enum deevee_content_type
{
   DEEVEE_CONTENT_NONE = 0,
   DEEVEE_CONTENT_ISO,
   DEEVEE_CONTENT_CHD,
   DEEVEE_CONTENT_IFO,
   DEEVEE_CONTENT_DVD_FOLDER
};

struct deevee_content_info
{
   enum deevee_content_type type;
   char path[DEEVEE_MAX_PATH];
};

bool deevee_content_probe(const char *path, struct deevee_content_info *info);
const char *deevee_content_type_name(enum deevee_content_type type);

#endif
