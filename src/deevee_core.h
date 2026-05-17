#ifndef DEEVEE_CORE_H
#define DEEVEE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deevee_audio.h"
#include "deevee_content.h"
#include "deevee_nav.h"
#include "deevee_video.h"

struct deevee_frame
{
   const void *pixels;
   unsigned width;
   unsigned height;
   size_t pitch;
};

struct deevee_audio_frame
{
   const int16_t *samples;
   size_t frames;
};

struct deevee_core
{
   bool initialized;
   bool loaded;
   uint64_t frame_count;
   struct deevee_content_info content;
   struct deevee_nav nav;
   struct deevee_video video;
   struct deevee_audio audio;
};

bool deevee_core_init(struct deevee_core *core);
void deevee_core_deinit(struct deevee_core *core);
bool deevee_core_load(struct deevee_core *core, const char *path);
void deevee_core_unload(struct deevee_core *core);
void deevee_core_reset(struct deevee_core *core);
void deevee_core_set_button(struct deevee_core *core,
      enum deevee_nav_button button, bool pressed);
void deevee_core_run(struct deevee_core *core, struct deevee_frame *video,
      struct deevee_audio_frame *audio);
const char *deevee_core_loaded_content_type(const struct deevee_core *core);

#endif
