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
   if (decoder->parser)
   {
      AVCodecParserContext *parser =
         (AVCodecParserContext *)decoder->parser;
      av_parser_close(parser);
   }
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
      AVCodecParserContext *parser;

      av_log_set_level(AV_LOG_QUIET);

      if (!codec)
         return DEEVEE_DECODER_ERROR_NOT_FOUND;

      parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
      if (!parser)
         return DEEVEE_DECODER_ERROR_OPEN_FAILED;

      context = avcodec_alloc_context3(codec);
      if (!context)
      {
         av_parser_close(parser);
         return DEEVEE_DECODER_ERROR_OPEN_FAILED;
      }
      context->pkt_timebase.num = 1;
      context->pkt_timebase.den = 90000;

      if (avcodec_open2(context, codec, NULL) < 0)
      {
         avcodec_free_context(&context);
         av_parser_close(parser);
         return DEEVEE_DECODER_ERROR_OPEN_FAILED;
      }

      decoder->context = context;
      decoder->parser = parser;
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

void deevee_decoder_set_frame_callback(struct deevee_video_decoder *decoder,
      deevee_decoder_frame_callback callback, void *user_data)
{
   if (!decoder)
      return;

   decoder->frame_callback = callback;
   decoder->frame_callback_user_data = user_data;
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
      probe->has_pts = false;
      probe->pts = 0;
      if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
      {
         probe->has_pts = true;
         probe->pts = frame->best_effort_timestamp;
      }
      if (decoder->frame_callback)
         decoder->frame_callback(decoder, probe,
               decoder->frame_callback_user_data);
      av_frame_unref(frame);
   }

   av_frame_free(&frame);

   if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
      return DEEVEE_DECODER_OK;

   return DEEVEE_DECODER_ERROR_DECODE_FAILED;
}

static enum deevee_decoder_status send_decoder_packet(
      struct deevee_video_decoder *decoder, const uint8_t *payload,
      size_t payload_size, bool has_pts, int64_t pts, bool has_dts,
      int64_t dts, struct deevee_decoder_frame_probe *probe)
{
   AVCodecContext *context;
   AVPacket *packet;
   int result;

   if (!decoder || !decoder->opened || !decoder->context || !payload ||
         !payload_size || !probe)
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
   if (has_pts)
      packet->pts = pts;
   if (has_dts)
      packet->dts = dts;

   result = avcodec_send_packet(context, packet);
   if (result == AVERROR(EAGAIN))
   {
      enum deevee_decoder_status receive_status =
         receive_available_frames(decoder, probe);
      if (receive_status != DEEVEE_DECODER_OK)
      {
         av_packet_free(&packet);
         return receive_status;
      }
      result = avcodec_send_packet(context, packet);
   }
   av_packet_free(&packet);

   if (result < 0)
      return DEEVEE_DECODER_ERROR_DECODE_FAILED;

   probe->packets_sent++;
   return receive_available_frames(decoder, probe);
}
#endif

enum deevee_decoder_status deevee_decoder_decode_mpeg2_payload(
      struct deevee_video_decoder *decoder, const uint8_t *payload,
      size_t payload_size, struct deevee_decoder_frame_probe *probe)
{
   return deevee_decoder_decode_mpeg2_timed_payload(decoder, payload,
         payload_size, false, 0, false, 0, probe);
}

enum deevee_decoder_status deevee_decoder_decode_mpeg2_timed_payload(
      struct deevee_video_decoder *decoder, const uint8_t *payload,
      size_t payload_size, bool has_pts, uint64_t pts, bool has_dts,
      uint64_t dts, struct deevee_decoder_frame_probe *probe)
{
   if (!decoder || !payload || !payload_size || !probe)
      return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      AVCodecContext *context;
      AVCodecParserContext *parser;
      const uint8_t *cursor = payload;
      int remaining = (int)payload_size;
      bool sent_timestamp = false;
      enum deevee_decoder_status status = DEEVEE_DECODER_OK;

      if (!decoder->opened || !decoder->context)
         return DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;

      context = (AVCodecContext *)decoder->context;
      parser = (AVCodecParserContext *)decoder->parser;
      if (!parser)
         return send_decoder_packet(decoder, payload, payload_size, has_pts,
               (int64_t)pts, has_dts, (int64_t)dts, probe);

      while (remaining > 0)
      {
         uint8_t *parsed_payload = NULL;
         int parsed_size = 0;
         int64_t input_pts = has_pts && !sent_timestamp ?
            (int64_t)pts : AV_NOPTS_VALUE;
         int64_t input_dts = has_dts && !sent_timestamp ?
            (int64_t)dts : AV_NOPTS_VALUE;
         int consumed = av_parser_parse2(parser, context, &parsed_payload,
               &parsed_size, cursor, remaining, input_pts, input_dts, 0);

         if (consumed < 0)
            return DEEVEE_DECODER_ERROR_DECODE_FAILED;

         cursor += consumed;
         remaining -= consumed;

         if (parsed_size > 0)
         {
            bool parsed_has_pts = parser->pts != AV_NOPTS_VALUE;
            bool parsed_has_dts = parser->dts != AV_NOPTS_VALUE;

            status = send_decoder_packet(decoder, parsed_payload,
                  (size_t)parsed_size, parsed_has_pts, parser->pts,
                  parsed_has_dts, parser->dts, probe);
            if (status != DEEVEE_DECODER_OK)
               return status;
            if (input_pts != AV_NOPTS_VALUE || input_dts != AV_NOPTS_VALUE)
               sent_timestamp = true;
         }

         if (consumed == 0 && parsed_size == 0)
            break;
      }

      return status;
   }
#else
   (void)payload;
   (void)payload_size;
   (void)has_pts;
   (void)pts;
   (void)has_dts;
   (void)dts;
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
