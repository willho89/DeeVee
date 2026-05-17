#ifndef DEEVEE_AUDIO_H
#define DEEVEE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define DEEVEE_AUDIO_SAMPLE_RATE 48000
#define DEEVEE_AUDIO_FRAMES_PER_RUN (DEEVEE_AUDIO_SAMPLE_RATE / 60)
#define DEEVEE_AUDIO_CHANNELS 2

struct deevee_audio
{
   int16_t silence[DEEVEE_AUDIO_FRAMES_PER_RUN * DEEVEE_AUDIO_CHANNELS];
};

void deevee_audio_init(struct deevee_audio *audio);
const int16_t *deevee_audio_silence(const struct deevee_audio *audio,
      size_t *frames);

#endif
