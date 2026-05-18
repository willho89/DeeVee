#ifndef DEEVEE_SUBPICTURE_H
#define DEEVEE_SUBPICTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEEVEE_SUBPICTURE_MAX_RECTS 8

enum deevee_subpicture_status
{
   DEEVEE_SUBPICTURE_OK = 0,
   DEEVEE_SUBPICTURE_ERROR_INVALID_ARGUMENT,
   DEEVEE_SUBPICTURE_ERROR_UNAVAILABLE,
   DEEVEE_SUBPICTURE_ERROR_OPEN_FAILED,
   DEEVEE_SUBPICTURE_ERROR_ALLOCATION_FAILED,
   DEEVEE_SUBPICTURE_ERROR_DECODE_FAILED
};

struct deevee_subpicture_rect
{
   int x;
   int y;
   int width;
   int height;
   uint32_t *pixels;
   uint8_t *indexes;
};

struct deevee_subpicture_frame
{
   bool valid;
   bool has_pts;
   int64_t pts;
   unsigned rect_count;
   struct deevee_subpicture_rect rects[DEEVEE_SUBPICTURE_MAX_RECTS];
};

struct deevee_subpicture_decoder
{
   bool initialized;
   bool opened;
   void *context;
};

void deevee_subpicture_init(struct deevee_subpicture_decoder *decoder);
void deevee_subpicture_deinit(struct deevee_subpicture_decoder *decoder);
enum deevee_subpicture_status deevee_subpicture_open_dvd(
      struct deevee_subpicture_decoder *decoder);
void deevee_subpicture_frame_clear(struct deevee_subpicture_frame *frame);
enum deevee_subpicture_status deevee_subpicture_decode_dvd_payload(
      struct deevee_subpicture_decoder *decoder, const uint8_t *payload,
      size_t payload_size, bool has_pts, int64_t pts,
      struct deevee_subpicture_frame *frame);
const char *deevee_subpicture_status_name(enum deevee_subpicture_status status);

#endif
