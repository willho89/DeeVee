#include "deevee_audio.h"

#include <string.h>

void deevee_audio_init(struct deevee_audio *audio)
{
   if (!audio)
      return;

   memset(audio->silence, 0, sizeof(audio->silence));
}

const int16_t *deevee_audio_silence(const struct deevee_audio *audio,
      size_t *frames)
{
   if (frames)
      *frames = DEEVEE_AUDIO_FRAMES_PER_RUN;

   return audio ? audio->silence : NULL;
}
