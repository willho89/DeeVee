#ifndef DEEVEE_DISC_H
#define DEEVEE_DISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deevee_content.h"

#define DEEVEE_DVD_SECTOR_SIZE 2048

enum deevee_disc_status
{
   DEEVEE_DISC_OK = 0,
   DEEVEE_DISC_ERROR_INVALID_ARGUMENT,
   DEEVEE_DISC_ERROR_UNSUPPORTED,
   DEEVEE_DISC_ERROR_OPEN_FAILED,
   DEEVEE_DISC_ERROR_SEEK_FAILED,
   DEEVEE_DISC_ERROR_READ_FAILED
};

struct deevee_disc
{
   void *handle;
   enum deevee_content_type type;
   uint64_t sector_count;
};

void deevee_disc_init(struct deevee_disc *disc);
void deevee_disc_close(struct deevee_disc *disc);
enum deevee_disc_status deevee_disc_open(struct deevee_disc *disc,
      const struct deevee_content_info *content);
enum deevee_disc_status deevee_disc_read_sector(struct deevee_disc *disc,
      uint64_t lba, uint8_t *out, size_t out_size);
const char *deevee_disc_status_name(enum deevee_disc_status status);

#endif
