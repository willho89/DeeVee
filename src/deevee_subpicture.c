#include "deevee_subpicture.h"

#include <stdlib.h>
#include <string.h>

#if HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#endif

void deevee_subpicture_init(struct deevee_subpicture_decoder *decoder)
{
   if (!decoder)
      return;

   memset(decoder, 0, sizeof(*decoder));
   decoder->initialized = true;
}

void deevee_subpicture_deinit(struct deevee_subpicture_decoder *decoder)
{
   if (!decoder)
      return;

#if HAVE_FFMPEG
   if (decoder->context)
   {
      AVCodecContext *context = (AVCodecContext *)decoder->context;
      avcodec_free_context(&context);
   }
#endif

   memset(decoder, 0, sizeof(*decoder));
}

enum deevee_subpicture_status deevee_subpicture_open_dvd(
      struct deevee_subpicture_decoder *decoder)
{
   if (!decoder || !decoder->initialized)
      return DEEVEE_SUBPICTURE_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_DVD_SUBTITLE);
      AVCodecContext *context;

      av_log_set_level(AV_LOG_QUIET);

      if (!codec)
         return DEEVEE_SUBPICTURE_ERROR_UNAVAILABLE;

      context = avcodec_alloc_context3(codec);
      if (!context)
         return DEEVEE_SUBPICTURE_ERROR_OPEN_FAILED;
      context->pkt_timebase.num = 1;
      context->pkt_timebase.den = 90000;

      if (avcodec_open2(context, codec, NULL) < 0)
      {
         avcodec_free_context(&context);
         return DEEVEE_SUBPICTURE_ERROR_OPEN_FAILED;
      }

      decoder->context = context;
      decoder->opened = true;
      return DEEVEE_SUBPICTURE_OK;
   }
#else
   (void)decoder;
   return DEEVEE_SUBPICTURE_ERROR_UNAVAILABLE;
#endif
}

void deevee_subpicture_frame_clear(struct deevee_subpicture_frame *frame)
{
   unsigned i;

   if (!frame)
      return;

   for (i = 0; i < DEEVEE_SUBPICTURE_MAX_RECTS; i++)
   {
      free(frame->rects[i].pixels);
      frame->rects[i].pixels = NULL;
      free(frame->rects[i].indexes);
      frame->rects[i].indexes = NULL;
   }
   memset(frame, 0, sizeof(*frame));
}

#if HAVE_FFMPEG
static uint32_t rgba_palette_to_xrgb(uint32_t rgba, uint8_t index,
      uint8_t *out_alpha)
{
   uint8_t r = (uint8_t)(rgba >> 24);
   uint8_t g = (uint8_t)(rgba >> 16);
   uint8_t b = (uint8_t)(rgba >> 8);
   uint8_t a = (uint8_t)rgba;

   if (!a && index)
   {
      r = 255;
      g = 224;
      b = 32;
      a = 224;
   }

   if (out_alpha)
      *out_alpha = a;
   return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
      ((uint32_t)g << 8) | b;
}

static enum deevee_subpicture_status copy_subtitle_bitmap_rect(
      const AVSubtitleRect *source, struct deevee_subpicture_rect *target)
{
   const uint8_t *indexes;
   const uint32_t *palette;
   int x;
   int y;
   uint32_t *pixels;
   uint8_t *target_indexes;

   if (!source || !target || source->type != SUBTITLE_BITMAP ||
         source->w <= 0 || source->h <= 0 || !source->data[0] ||
         !source->data[1] || source->linesize[0] <= 0)
      return DEEVEE_SUBPICTURE_OK;

   pixels = (uint32_t *)calloc((size_t)source->w * (size_t)source->h,
         sizeof(uint32_t));
   if (!pixels)
      return DEEVEE_SUBPICTURE_ERROR_ALLOCATION_FAILED;
   target_indexes = (uint8_t *)calloc((size_t)source->w * (size_t)source->h,
         sizeof(uint8_t));
   if (!target_indexes)
   {
      free(pixels);
      return DEEVEE_SUBPICTURE_ERROR_ALLOCATION_FAILED;
   }

   indexes = source->data[0];
   palette = (const uint32_t *)source->data[1];
   for (y = 0; y < source->h; y++)
   {
      for (x = 0; x < source->w; x++)
      {
         uint8_t alpha = 0;
         uint8_t index = indexes[y * source->linesize[0] + x];
         uint32_t pixel = rgba_palette_to_xrgb(palette[index], index,
               &alpha);

         target_indexes[(size_t)y * (size_t)source->w + (size_t)x] = index;
         if (alpha)
            pixels[(size_t)y * (size_t)source->w + (size_t)x] = pixel;
      }
   }

   target->x = source->x;
   target->y = source->y;
   target->width = source->w;
   target->height = source->h;
   target->pixels = pixels;
   target->indexes = target_indexes;
   return DEEVEE_SUBPICTURE_OK;
}
#endif

enum deevee_subpicture_status deevee_subpicture_decode_dvd_payload(
      struct deevee_subpicture_decoder *decoder, const uint8_t *payload,
      size_t payload_size, bool has_pts, int64_t pts,
      struct deevee_subpicture_frame *frame)
{
   if (!decoder || !payload || !payload_size || !frame)
      return DEEVEE_SUBPICTURE_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      AVCodecContext *context;
      AVPacket *packet;
      AVSubtitle subtitle;
      int got_subtitle = 0;
      int result;
      unsigned i;

      if (!decoder->opened || !decoder->context)
         return DEEVEE_SUBPICTURE_ERROR_INVALID_ARGUMENT;

      context = (AVCodecContext *)decoder->context;
      packet = av_packet_alloc();
      if (!packet)
         return DEEVEE_SUBPICTURE_ERROR_ALLOCATION_FAILED;

      result = av_new_packet(packet, (int)payload_size);
      if (result < 0)
      {
         av_packet_free(&packet);
         return DEEVEE_SUBPICTURE_ERROR_ALLOCATION_FAILED;
      }

      memcpy(packet->data, payload, payload_size);
      if (has_pts)
         packet->pts = pts;

      memset(&subtitle, 0, sizeof(subtitle));
      result = avcodec_decode_subtitle2(context, &subtitle, &got_subtitle,
            packet);
      av_packet_free(&packet);
      if (result < 0)
         return DEEVEE_SUBPICTURE_ERROR_DECODE_FAILED;

      if (!got_subtitle || !subtitle.num_rects)
      {
         avsubtitle_free(&subtitle);
         return DEEVEE_SUBPICTURE_OK;
      }

      deevee_subpicture_frame_clear(frame);
      frame->valid = true;
      frame->has_pts = has_pts;
      frame->pts = pts;

      for (i = 0; i < subtitle.num_rects &&
            frame->rect_count < DEEVEE_SUBPICTURE_MAX_RECTS; i++)
      {
         struct deevee_subpicture_rect *target =
            &frame->rects[frame->rect_count];
         enum deevee_subpicture_status copy_status =
            copy_subtitle_bitmap_rect(subtitle.rects[i], target);

         if (copy_status != DEEVEE_SUBPICTURE_OK)
         {
            avsubtitle_free(&subtitle);
            return copy_status;
         }
         if (target->pixels)
            frame->rect_count++;
      }

      if (!frame->rect_count)
         frame->valid = false;
      avsubtitle_free(&subtitle);
      return DEEVEE_SUBPICTURE_OK;
   }
#else
   (void)decoder;
   (void)payload;
   (void)payload_size;
   (void)has_pts;
   (void)pts;
   (void)frame;
   return DEEVEE_SUBPICTURE_ERROR_UNAVAILABLE;
#endif
}

const char *deevee_subpicture_status_name(enum deevee_subpicture_status status)
{
   switch (status)
   {
      case DEEVEE_SUBPICTURE_OK:
         return "ok";
      case DEEVEE_SUBPICTURE_ERROR_INVALID_ARGUMENT:
         return "invalid_argument";
      case DEEVEE_SUBPICTURE_ERROR_UNAVAILABLE:
         return "unavailable";
      case DEEVEE_SUBPICTURE_ERROR_OPEN_FAILED:
         return "open_failed";
      case DEEVEE_SUBPICTURE_ERROR_ALLOCATION_FAILED:
         return "allocation_failed";
      case DEEVEE_SUBPICTURE_ERROR_DECODE_FAILED:
         return "decode_failed";
      default:
         return "unknown";
   }
}
