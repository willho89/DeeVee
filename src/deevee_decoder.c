#include "deevee_decoder.h"

#include <string.h>

#if HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
#endif

enum deevee_decoder_status deevee_decoder_init(
      struct deevee_video_decoder *decoder)
{
   if (!decoder)
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

   memset(decoder, 0, sizeof(*decoder));
   decoder->backend_name = deevee_decoder_backend_name();
   decoder->initialized = true;
   return DEEVEE_DECODER_OK;
}

void deevee_decoder_deinit(struct deevee_video_decoder *decoder)
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

enum deevee_decoder_status deevee_decoder_open_mpeg2(
      struct deevee_video_decoder *decoder)
{
   if (!decoder || !decoder->initialized)
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
      AVCodecContext *context;

      av_log_set_level(AV_LOG_QUIET);

      if (!codec)
         return DEEVEE_DECODER_ERROR_NOT_FOUND;

      context = avcodec_alloc_context3(codec);
      if (!context)
         return DEEVEE_DECODER_ERROR_OPEN_FAILED;

      if (avcodec_open2(context, codec, NULL) < 0)
      {
         avcodec_free_context(&context);
         return DEEVEE_DECODER_ERROR_OPEN_FAILED;
      }

      decoder->context = context;
      decoder->opened = true;
      return DEEVEE_DECODER_OK;
   }
#else
   (void)decoder;
   return DEEVEE_DECODER_ERROR_UNAVAILABLE;
#endif
}

enum deevee_decoder_status deevee_decoder_set_xrgb8888_output(
      struct deevee_video_decoder *decoder, uint32_t *pixels,
      unsigned width, unsigned height, size_t pitch)
{
   if (!decoder || !decoder->initialized || !pixels || !width || !height ||
         pitch < (size_t)width * sizeof(uint32_t))
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

   decoder->output_pixels = pixels;
   decoder->output_width = width;
   decoder->output_height = height;
   decoder->output_pitch = pitch;
   decoder->output_ready = false;
   return DEEVEE_DECODER_OK;
}

#if HAVE_FFMPEG
static enum deevee_decoder_status copy_frame_to_output(
      struct deevee_video_decoder *decoder, const AVFrame *frame)
{
   struct SwsContext *scale_context;
   uint8_t *dst_data[4];
   int dst_linesize[4];
   int result;

   if (!decoder || !frame || !decoder->output_pixels)
      return DEEVEE_DECODER_OK;

   scale_context = sws_getContext(frame->width, frame->height,
         (enum AVPixelFormat)frame->format,
         (int)decoder->output_width, (int)decoder->output_height,
         AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
   if (!scale_context)
      return DEEVEE_DECODER_ERROR_ALLOCATION_FAILED;

   dst_data[0] = (uint8_t *)decoder->output_pixels;
   dst_data[1] = NULL;
   dst_data[2] = NULL;
   dst_data[3] = NULL;
   dst_linesize[0] = (int)decoder->output_pitch;
   dst_linesize[1] = 0;
   dst_linesize[2] = 0;
   dst_linesize[3] = 0;

   result = sws_scale(scale_context, (const uint8_t * const *)frame->data,
         frame->linesize, 0, frame->height, dst_data, dst_linesize);
   sws_freeContext(scale_context);

   if (result <= 0)
      return DEEVEE_DECODER_ERROR_DECODE_FAILED;

   decoder->output_ready = true;
   return DEEVEE_DECODER_OK;
}

static enum deevee_decoder_status receive_available_frames(
      struct deevee_video_decoder *decoder,
      struct deevee_decoder_frame_probe *probe)
{
   AVCodecContext *context;
   AVFrame *frame;
   int result;

   if (!decoder || !decoder->opened || !decoder->context || !probe)
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

   context = (AVCodecContext *)decoder->context;
   frame = av_frame_alloc();
   if (!frame)
      return DEEVEE_DECODER_ERROR_ALLOCATION_FAILED;

   while ((result = avcodec_receive_frame(context, frame)) == 0)
   {
      enum deevee_decoder_status copy_status =
         copy_frame_to_output(decoder, frame);
      if (copy_status != DEEVEE_DECODER_OK)
      {
         av_frame_free(&frame);
         return copy_status;
      }

      probe->frames_decoded++;
      probe->got_frame = true;
      probe->width = (unsigned)frame->width;
      probe->height = (unsigned)frame->height;
      probe->pixel_format = frame->format;
      av_frame_unref(frame);
   }

   av_frame_free(&frame);

   if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
      return DEEVEE_DECODER_OK;

   return DEEVEE_DECODER_ERROR_DECODE_FAILED;
}
#endif

enum deevee_decoder_status deevee_decoder_decode_mpeg2_payload(
      struct deevee_video_decoder *decoder, const uint8_t *payload,
      size_t payload_size, struct deevee_decoder_frame_probe *probe)
{
   if (!decoder || !payload || !payload_size || !probe)
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      AVCodecContext *context;
      AVPacket *packet;
      int result;

      if (!decoder->opened || !decoder->context)
         return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

      context = (AVCodecContext *)decoder->context;
      packet = av_packet_alloc();
      if (!packet)
         return DEEVEE_DECODER_ERROR_ALLOCATION_FAILED;

      result = av_new_packet(packet, (int)payload_size);
      if (result < 0)
      {
         av_packet_free(&packet);
         return DEEVEE_DECODER_ERROR_ALLOCATION_FAILED;
      }

      memcpy(packet->data, payload, payload_size);
      result = avcodec_send_packet(context, packet);
      av_packet_free(&packet);

      if (result < 0)
         return DEEVEE_DECODER_ERROR_DECODE_FAILED;

      probe->packets_sent++;
      return receive_available_frames(decoder, probe);
   }
#else
   (void)payload;
   (void)payload_size;
   return DEEVEE_DECODER_ERROR_UNAVAILABLE;
#endif
}

enum deevee_decoder_status deevee_decoder_flush_mpeg2(
      struct deevee_video_decoder *decoder,
      struct deevee_decoder_frame_probe *probe)
{
   if (!decoder || !probe)
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      AVCodecContext *context;
      int result;

      if (!decoder->opened || !decoder->context)
         return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

      context = (AVCodecContext *)decoder->context;
      result = avcodec_send_packet(context, NULL);
      if (result < 0 && result != AVERROR_EOF)
         return DEEVEE_DECODER_ERROR_DECODE_FAILED;

      return receive_available_frames(decoder, probe);
   }
#else
   return DEEVEE_DECODER_ERROR_UNAVAILABLE;
#endif
}

bool deevee_decoder_is_available(void)
{
#if HAVE_FFMPEG
   return avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO) != NULL;
#else
   return false;
#endif
}

const char *deevee_decoder_backend_name(void)
{
#if HAVE_FFMPEG
   return "ffmpeg";
#else
   return "none";
#endif
}

const char *deevee_decoder_status_name(enum deevee_decoder_status status)
{
   switch (status)
   {
      case DEEVEE_DECODER_OK:
         return "ok";
      case DEEVEE_DECODER_ERROR_INVALID_ARGUMENT:
         return "invalid_argument";
      case DEEVEE_DECODER_ERROR_UNAVAILABLE:
         return "unavailable";
      case DEEVEE_DECODER_ERROR_NOT_FOUND:
         return "not_found";
      case DEEVEE_DECODER_ERROR_OPEN_FAILED:
         return "open_failed";
      case DEEVEE_DECODER_ERROR_ALLOCATION_FAILED:
         return "allocation_failed";
      case DEEVEE_DECODER_ERROR_DECODE_FAILED:
         return "decode_failed";
      default:
         return "unknown";
   }
}
