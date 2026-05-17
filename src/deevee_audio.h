#ifndef DEEVEE_AUDIO_H
#define DEEVEE_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DEEVEE_AUDIO_SAMPLE_RATE 48000
#define DEEVEE_AUDIO_FRAMES_PER_RUN (DEEVEE_AUDIO_SAMPLE_RATE / 60)
#define DEEVEE_AUDIO_CHANNELS 2
#define DEEVEE_AUDIO_BUFFER_FRAMES (DEEVEE_AUDIO_SAMPLE_RATE * 4)

enum deevee_audio_status
{
   DEEVEE_AUDIO_OK = 0,
   DEEVEE_AUDIO_ERROR_INVALID_ARGUMENT,
   DEEVEE_AUDIO_ERROR_UNAVAILABLE,
   DEEVEE_AUDIO_ERROR_OPEN_FAILED,
   DEEVEE_AUDIO_ERROR_ALLOCATION_FAILED,
   DEEVEE_AUDIO_ERROR_DECODE_FAILED
};

struct deevee_audio_decoder
{
   bool initialized;
   bool opened;
   void *context;
   void *parser;
};

struct deevee_audio
{
   int16_t silence[DEEVEE_AUDIO_FRAMES_PER_RUN * DEEVEE_AUDIO_CHANNELS];
   int16_t buffer[DEEVEE_AUDIO_BUFFER_FRAMES * DEEVEE_AUDIO_CHANNELS];
   size_t read_pos;
   size_t write_pos;
   size_t buffered_frames;
   struct deevee_audio_decoder decoder;
   bool decoder_ready;
   uint64_t packets_sent;
   uint64_t decoded_frames;
   uint64_t decode_errors;
   uint64_t underruns;
   unsigned last_sample_rate;
   unsigned last_channels;
};

void deevee_audio_init(struct deevee_audio *audio);
void deevee_audio_deinit(struct deevee_audio *audio);
void deevee_audio_reset(struct deevee_audio *audio);
enum deevee_audio_status deevee_audio_open_ac3(struct deevee_audio *audio);
enum deevee_audio_status deevee_audio_decode_ac3_payload(
      struct deevee_audio *audio, const uint8_t *payload,
      size_t payload_size);
const int16_t *deevee_audio_read(struct deevee_audio *audio, size_t *frames);
const int16_t *deevee_audio_silence(const struct deevee_audio *audio,
      size_t *frames);
size_t deevee_audio_buffered_frames(const struct deevee_audio *audio);
uint64_t deevee_audio_packets_sent(const struct deevee_audio *audio);
uint64_t deevee_audio_decoded_frames(const struct deevee_audio *audio);
uint64_t deevee_audio_decode_errors(const struct deevee_audio *audio);
uint64_t deevee_audio_underruns(const struct deevee_audio *audio);
unsigned deevee_audio_last_sample_rate(const struct deevee_audio *audio);
unsigned deevee_audio_last_channels(const struct deevee_audio *audio);

#endif
