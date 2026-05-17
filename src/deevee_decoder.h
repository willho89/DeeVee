#ifndef DEEVEE_DECODER_H
#define DEEVEE_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum deevee_decoder_status
{
   DEEVEE_DECODER_OK = 0,
   DEEVEE_DECODER_ERROR_INVALID_ARGUMENT,
   DEEVEE_DECODER_ERROR_UNAVAILABLE,
   DEEVEE_DECODER_ERROR_NOT_FOUND,
   DEEVEE_DECODER_ERROR_OPEN_FAILED,
   DEEVEE_DECODER_ERROR_ALLOCATION_FAILED,
   DEEVEE_DECODER_ERROR_DECODE_FAILED
};

struct deevee_video_decoder
{
   bool initialized;
   bool opened;
   const char *backend_name;
   void *context;
   void *parser;
   uint32_t *output_pixels;
   unsigned output_width;
   unsigned output_height;
   size_t output_pitch;
   bool output_ready;
};

struct deevee_decoder_frame_probe
{
   uint32_t packets_sent;
   uint32_t frames_decoded;
   unsigned width;
   unsigned height;
   int pixel_format;
   bool got_frame;
   bool has_pts;
   int64_t pts;
};

enum deevee_decoder_status deevee_decoder_init(
      struct deevee_video_decoder *decoder);
void deevee_decoder_deinit(struct deevee_video_decoder *decoder);
enum deevee_decoder_status deevee_decoder_open_mpeg2(
      struct deevee_video_decoder *decoder);
enum deevee_decoder_status deevee_decoder_set_xrgb8888_output(
      struct deevee_video_decoder *decoder, uint32_t *pixels,
      unsigned width, unsigned height, size_t pitch);
enum deevee_decoder_status deevee_decoder_decode_mpeg2_payload(
      struct deevee_video_decoder *decoder, const uint8_t *payload,
      size_t payload_size, struct deevee_decoder_frame_probe *probe);
enum deevee_decoder_status deevee_decoder_decode_mpeg2_timed_payload(
      struct deevee_video_decoder *decoder, const uint8_t *payload,
      size_t payload_size, bool has_pts, uint64_t pts, bool has_dts,
      uint64_t dts, struct deevee_decoder_frame_probe *probe);
enum deevee_decoder_status deevee_decoder_flush_mpeg2(
      struct deevee_video_decoder *decoder,
      struct deevee_decoder_frame_probe *probe);
bool deevee_decoder_is_available(void);
const char *deevee_decoder_backend_name(void);
const char *deevee_decoder_status_name(enum deevee_decoder_status status);

#endif
