#ifndef DEEVEE_ISO_H
#define DEEVEE_ISO_H

#include <stdbool.h>
#include <stdint.h>

#include "deevee_disc.h"

enum deevee_iso_status
{
   DEEVEE_ISO_OK = 0,
   DEEVEE_ISO_ERROR_INVALID_ARGUMENT,
   DEEVEE_ISO_ERROR_NOT_ISO9660,
   DEEVEE_ISO_ERROR_NOT_FOUND,
   DEEVEE_ISO_ERROR_READ_FAILED,
   DEEVEE_ISO_ERROR_MALFORMED
};

struct deevee_iso_entry
{
   uint32_t lba;
   uint32_t size;
   bool is_directory;
};

enum deevee_iso_status deevee_iso_find_path(struct deevee_disc *disc,
      const char *path, struct deevee_iso_entry *entry);
const char *deevee_iso_status_name(enum deevee_iso_status status);

#endif
