#include "deevee_core.h"

#include <string.h>

bool deevee_core_init(struct deevee_core *core)
{
   if (!core)
      return false;

   memset(core, 0, sizeof(*core));
   deevee_nav_init(&core->nav);
   deevee_audio_init(&core->audio);

   if (!deevee_video_init(&core->video))
      return false;

   core->initialized = true;
   return true;
}

void deevee_core_deinit(struct deevee_core *core)
{
   if (!core)
      return;

   deevee_video_deinit(&core->video);
   memset(core, 0, sizeof(*core));
}

bool deevee_core_load(struct deevee_core *core, const char *path)
{
   struct deevee_content_info content;

   if (!core || !core->initialized)
      return false;

   if (!deevee_content_probe(path, &content))
      return false;

   core->content = content;
   core->loaded = true;
   core->frame_count = 0;
   deevee_nav_init(&core->nav);
   return true;
}

void deevee_core_unload(struct deevee_core *core)
{
   if (!core)
      return;

   core->loaded = false;
   core->frame_count = 0;
   memset(&core->content, 0, sizeof(core->content));
   deevee_nav_init(&core->nav);
}

void deevee_core_reset(struct deevee_core *core)
{
   if (!core)
      return;

   core->frame_count = 0;
   deevee_nav_init(&core->nav);
}

void deevee_core_set_button(struct deevee_core *core,
      enum deevee_nav_button button, bool pressed)
{
   if (!core)
      return;

   deevee_nav_set_button(&core->nav, button, pressed);
}

void deevee_core_run(struct deevee_core *core, struct deevee_frame *video,
      struct deevee_audio_frame *audio)
{
   const char *label = "no content";

   if (!core || !video || !audio)
      return;

   if (core->loaded)
      label = deevee_content_type_name(core->content.type);

   deevee_video_render_placeholder(&core->video, core->frame_count, label);

   video->pixels = core->video.pixels;
   video->width = DEEVEE_VIDEO_WIDTH;
   video->height = DEEVEE_VIDEO_HEIGHT;
   video->pitch = core->video.pitch;
   audio->samples = deevee_audio_silence(&core->audio, &audio->frames);

   core->frame_count++;
}

const char *deevee_core_loaded_content_type(const struct deevee_core *core)
{
   if (!core || !core->loaded)
      return deevee_content_type_name(DEEVEE_CONTENT_NONE);

   return deevee_content_type_name(core->content.type);
}
