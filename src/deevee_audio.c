#include "deevee_audio.h"

#include <stdlib.h>
#include <string.h>

#if HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#endif

#if HAVE_FFMPEG
static int16_t clamp_s16(float value)
{
   if (value > 32767.0f)
      return 32767;
   if (value < -32768.0f)
      return -32768;
   return (int16_t)value;
}
#endif

void deevee_audio_init(struct deevee_audio *audio)
{
   if (!audio)
      return;

   memset(audio, 0, sizeof(*audio));
}

void deevee_audio_deinit(struct deevee_audio *audio)
{
   if (!audio)
      return;

#if HAVE_FFMPEG
   if (audio->decoder.parser)
   {
      AVCodecParserContext *parser =
         (AVCodecParserContext *)audio->decoder.parser;
      av_parser_close(parser);
   }
   if (audio->decoder.context)
   {
      AVCodecContext *context = (AVCodecContext *)audio->decoder.context;
      avcodec_free_context(&context);
   }
#endif

   memset(audio, 0, sizeof(*audio));
}

void deevee_audio_reset(struct deevee_audio *audio)
{
   uint64_t underruns;

   if (!audio)
      return;

   underruns = audio->underruns;
   audio->read_pos = 0;
   audio->write_pos = 0;
   audio->buffered_frames = 0;
   audio->packets_sent = 0;
   audio->decoded_frames = 0;
   audio->decode_errors = 0;
   audio->underruns = underruns;
   memset(audio->buffer, 0, sizeof(audio->buffer));
}

const int16_t *deevee_audio_silence(const struct deevee_audio *audio,
      size_t *frames)
{
   if (frames)
      *frames = DEEVEE_AUDIO_FRAMES_PER_RUN;

   return audio ? audio->silence : NULL;
}

const int16_t *deevee_audio_read(struct deevee_audio *audio, size_t *frames)
{
   size_t i;

   if (frames)
      *frames = DEEVEE_AUDIO_FRAMES_PER_RUN;

   if (!audio)
      return NULL;

   if (audio->buffered_frames < DEEVEE_AUDIO_FRAMES_PER_RUN)
   {
      audio->underruns++;
      return deevee_audio_silence(audio, frames);
   }

   for (i = 0; i < DEEVEE_AUDIO_FRAMES_PER_RUN; i++)
   {
      size_t src = audio->read_pos * DEEVEE_AUDIO_CHANNELS;
      size_t dst = i * DEEVEE_AUDIO_CHANNELS;

      audio->silence[dst] = audio->buffer[src];
      audio->silence[dst + 1u] = audio->buffer[src + 1u];
      audio->read_pos = (audio->read_pos + 1u) % DEEVEE_AUDIO_BUFFER_FRAMES;
      audio->buffered_frames--;
   }

   return audio->silence;
}

#if HAVE_FFMPEG
static void push_audio_frame(struct deevee_audio *audio, int16_t left,
      int16_t right)
{
   size_t sample_index;

   if (!audio)
      return;

   if (audio->buffered_frames >= DEEVEE_AUDIO_BUFFER_FRAMES)
   {
      audio->read_pos = (audio->read_pos + 1u) % DEEVEE_AUDIO_BUFFER_FRAMES;
      audio->buffered_frames--;
   }

   sample_index = audio->write_pos * DEEVEE_AUDIO_CHANNELS;
   audio->buffer[sample_index] = left;
   audio->buffer[sample_index + 1u] = right;
   audio->write_pos = (audio->write_pos + 1u) % DEEVEE_AUDIO_BUFFER_FRAMES;
   audio->buffered_frames++;
}

static void push_silence(struct deevee_audio *audio, size_t frames)
{
   size_t i;

   for (i = 0; i < frames; i++)
      push_audio_frame(audio, 0, 0);
}

static int frame_channels(const AVFrame *frame)
{
#if LIBAVUTIL_VERSION_MAJOR >= 57
   return frame->ch_layout.nb_channels;
#else
   return frame->channels;
#endif
}

static float sample_as_float(const AVFrame *frame, int channel, int index)
{
   enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
   const uint8_t *plane;

   if (channel >= frame_channels(frame))
      channel = 0;

   switch (format)
   {
      case AV_SAMPLE_FMT_FLTP:
         return ((const float *)frame->data[channel])[index] * 32767.0f;
      case AV_SAMPLE_FMT_FLT:
         return ((const float *)frame->data[0])[
            index * frame_channels(frame) + channel] * 32767.0f;
      case AV_SAMPLE_FMT_S16P:
         return (float)((const int16_t *)frame->data[channel])[index];
      case AV_SAMPLE_FMT_S16:
         return (float)((const int16_t *)frame->data[0])[
            index * frame_channels(frame) + channel];
      case AV_SAMPLE_FMT_S32P:
         return (float)(((const int32_t *)frame->data[channel])[index] >> 16);
      case AV_SAMPLE_FMT_S32:
         return (float)(((const int32_t *)frame->data[0])[
            index * frame_channels(frame) + channel] >> 16);
      case AV_SAMPLE_FMT_U8P:
         plane = frame->data[channel];
         return ((float)plane[index] - 128.0f) * 256.0f;
      case AV_SAMPLE_FMT_U8:
         plane = frame->data[0];
         return ((float)plane[index * frame_channels(frame) + channel] -
            128.0f) * 256.0f;
      default:
         return 0.0f;
   }
}

static void downmix_frame_sample(const AVFrame *frame, int index,
      int16_t *out_left, int16_t *out_right)
{
   int channels = frame_channels(frame);
   float left;
   float right;

   if (!out_left || !out_right)
      return;

   if (channels <= 0)
   {
      *out_left = 0;
      *out_right = 0;
      return;
   }

   left = sample_as_float(frame, 0, index);
   right = channels > 1 ? sample_as_float(frame, 1, index) : left;

   if (channels > 2)
   {
      float center = sample_as_float(frame, 2, index) * 0.7071f;
      left += center;
      right += center;
   }

   if (channels > 3)
   {
      float lfe = sample_as_float(frame, 3, index) * 0.25f;
      left += lfe;
      right += lfe;
   }

   if (channels > 4)
      left += sample_as_float(frame, 4, index) * 0.5f;
   if (channels > 5)
      right += sample_as_float(frame, 5, index) * 0.5f;

   *out_left = clamp_s16(left);
   *out_right = clamp_s16(right);
}

static enum deevee_audio_status receive_audio_frames(struct deevee_audio *audio)
{
   AVCodecContext *context;
   AVFrame *frame;
   int result;

   if (!audio || !audio->decoder.context)
      return DEEVEE_AUDIO_ERROR_INVALID_ARGUMENT;

   context = (AVCodecContext *)audio->decoder.context;
   frame = av_frame_alloc();
   if (!frame)
      return DEEVEE_AUDIO_ERROR_ALLOCATION_FAILED;

   while ((result = avcodec_receive_frame(context, frame)) == 0)
   {
      int i;
      int channels = frame_channels(frame);

      audio->last_sample_rate = (unsigned)frame->sample_rate;
      audio->last_channels = (unsigned)(channels > 0 ? channels : 0);
      if (frame->sample_rate != DEEVEE_AUDIO_SAMPLE_RATE)
         push_silence(audio, (size_t)frame->nb_samples);
      else
      {
         for (i = 0; i < frame->nb_samples; i++)
         {
            int16_t left;
            int16_t right;

            downmix_frame_sample(frame, i, &left, &right);
            push_audio_frame(audio, left, right);
         }
      }

      audio->decoded_frames += (uint64_t)frame->nb_samples;
      av_frame_unref(frame);
   }

   av_frame_free(&frame);
   if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
      return DEEVEE_AUDIO_OK;

   return DEEVEE_AUDIO_ERROR_DECODE_FAILED;
}
#endif

enum deevee_audio_status deevee_audio_open_ac3(struct deevee_audio *audio)
{
   if (!audio)
      return DEEVEE_AUDIO_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AC3);
      AVCodecContext *context;
      AVCodecParserContext *parser;

      if (audio->decoder_ready)
         return DEEVEE_AUDIO_OK;

      if (!codec)
         return DEEVEE_AUDIO_ERROR_UNAVAILABLE;

      parser = av_parser_init(AV_CODEC_ID_AC3);
      if (!parser)
         return DEEVEE_AUDIO_ERROR_OPEN_FAILED;

      context = avcodec_alloc_context3(codec);
      if (!context)
      {
         av_parser_close(parser);
         return DEEVEE_AUDIO_ERROR_ALLOCATION_FAILED;
      }
      context->pkt_timebase.num = 1;
      context->pkt_timebase.den = 90000;

      if (avcodec_open2(context, codec, NULL) < 0)
      {
         avcodec_free_context(&context);
         av_parser_close(parser);
         return DEEVEE_AUDIO_ERROR_OPEN_FAILED;
      }

      audio->decoder.context = context;
      audio->decoder.parser = parser;
      audio->decoder.initialized = true;
      audio->decoder.opened = true;
      audio->decoder_ready = true;
      return DEEVEE_AUDIO_OK;
   }
#else
   return DEEVEE_AUDIO_ERROR_UNAVAILABLE;
#endif
}

#if HAVE_FFMPEG
static enum deevee_audio_status send_audio_packet(struct deevee_audio *audio,
      const uint8_t *payload, size_t payload_size)
{
   AVCodecContext *context;
   AVPacket *packet;
   int result;

   if (!audio || !audio->decoder.context || !payload || !payload_size)
      return DEEVEE_AUDIO_ERROR_INVALID_ARGUMENT;

   context = (AVCodecContext *)audio->decoder.context;
   packet = av_packet_alloc();
   if (!packet)
      return DEEVEE_AUDIO_ERROR_ALLOCATION_FAILED;

   result = av_new_packet(packet, (int)payload_size);
   if (result < 0)
   {
      av_packet_free(&packet);
      return DEEVEE_AUDIO_ERROR_ALLOCATION_FAILED;
   }

   memcpy(packet->data, payload, payload_size);
   result = avcodec_send_packet(context, packet);
   if (result == AVERROR(EAGAIN))
   {
      enum deevee_audio_status receive_status = receive_audio_frames(audio);
      if (receive_status != DEEVEE_AUDIO_OK)
      {
         av_packet_free(&packet);
         return receive_status;
      }
      result = avcodec_send_packet(context, packet);
   }
   av_packet_free(&packet);

   if (result < 0)
      return DEEVEE_AUDIO_ERROR_DECODE_FAILED;

   audio->packets_sent++;
   return receive_audio_frames(audio);
}
#endif

enum deevee_audio_status deevee_audio_decode_ac3_payload(
      struct deevee_audio *audio, const uint8_t *payload,
      size_t payload_size)
{
   if (!audio || !payload || !payload_size)
      return DEEVEE_AUDIO_ERROR_INVALID_ARGUMENT;

#if HAVE_FFMPEG
   {
      AVCodecContext *context;
      AVCodecParserContext *parser;
      const uint8_t *cursor = payload;
      int remaining = (int)payload_size;
      enum deevee_audio_status status = DEEVEE_AUDIO_OK;

      status = deevee_audio_open_ac3(audio);
      if (status != DEEVEE_AUDIO_OK)
         return status;

      context = (AVCodecContext *)audio->decoder.context;
      parser = (AVCodecParserContext *)audio->decoder.parser;
      while (remaining > 0)
      {
         uint8_t *parsed_payload = NULL;
         int parsed_size = 0;
         int consumed = av_parser_parse2(parser, context, &parsed_payload,
               &parsed_size, cursor, remaining, AV_NOPTS_VALUE,
               AV_NOPTS_VALUE, 0);

         if (consumed < 0)
            return DEEVEE_AUDIO_ERROR_DECODE_FAILED;

         cursor += consumed;
         remaining -= consumed;
         if (parsed_size > 0)
         {
            status = send_audio_packet(audio, parsed_payload,
                  (size_t)parsed_size);
            if (status != DEEVEE_AUDIO_OK)
               return status;
         }

         if (consumed == 0 && parsed_size == 0)
            break;
      }

      return status;
   }
#else
   (void)payload;
   (void)payload_size;
   return DEEVEE_AUDIO_ERROR_UNAVAILABLE;
#endif
}

size_t deevee_audio_buffered_frames(const struct deevee_audio *audio)
{
   return audio ? audio->buffered_frames : 0;
}

uint64_t deevee_audio_packets_sent(const struct deevee_audio *audio)
{
   return audio ? audio->packets_sent : 0;
}

uint64_t deevee_audio_decoded_frames(const struct deevee_audio *audio)
{
   return audio ? audio->decoded_frames : 0;
}

uint64_t deevee_audio_decode_errors(const struct deevee_audio *audio)
{
   return audio ? audio->decode_errors : 0;
}

uint64_t deevee_audio_underruns(const struct deevee_audio *audio)
{
   return audio ? audio->underruns : 0;
}

unsigned deevee_audio_last_sample_rate(const struct deevee_audio *audio)
{
   return audio ? audio->last_sample_rate : 0;
}

unsigned deevee_audio_last_channels(const struct deevee_audio *audio)
{
   return audio ? audio->last_channels : 0;
}
