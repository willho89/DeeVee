#ifndef DEEVEE_VIDEO_H
#define DEEVEE_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEEVEE_VIDEO_WIDTH 720
#define DEEVEE_VIDEO_HEIGHT 480
#define DEEVEE_VIDEO_FPS 60.0

struct deevee_video
{
   uint32_t *pixels;
   size_t pitch;
};

bool deevee_video_init(struct deevee_video *video);
void deevee_video_deinit(struct deevee_video *video);
void deevee_video_render_placeholder(struct deevee_video *video,
      uint64_t frame_count, const char *content_label);
void deevee_video_render_decoded_overlay(struct deevee_video *video,
      uint64_t frame_count);

#endif
