#include "deevee_video.h"

#include <stdlib.h>
#include <string.h>

static uint32_t deevee_rgb(unsigned r, unsigned g, unsigned b)
{
   return 0xff000000u | ((r & 0xffu) << 16) |
      ((g & 0xffu) << 8) | (b & 0xffu);
}

bool deevee_video_init(struct deevee_video *video)
{
   size_t pixel_count = DEEVEE_VIDEO_WIDTH * DEEVEE_VIDEO_HEIGHT;

   if (!video)
      return false;

   memset(video, 0, sizeof(*video));
   video->pitch = DEEVEE_VIDEO_WIDTH * sizeof(uint32_t);
   video->pixels = (uint32_t *)calloc(pixel_count, sizeof(uint32_t));

   return video->pixels != NULL;
}

void deevee_video_deinit(struct deevee_video *video)
{
   if (!video)
      return;

   free(video->pixels);
   memset(video, 0, sizeof(*video));
}

void deevee_video_render_placeholder(struct deevee_video *video,
      uint64_t frame_count, const char *content_label)
{
   unsigned x;
   unsigned y;
   unsigned nav_bar;
   uint32_t accent;
   size_t label_hash = 0;

   if (!video || !video->pixels)
      return;

   if (content_label)
   {
      const char *cursor = content_label;
      while (*cursor)
         label_hash = (label_hash * 33u) ^ (unsigned char)*cursor++;
   }

   accent = deevee_rgb((unsigned)(label_hash + frame_count) & 0xffu,
         (unsigned)(label_hash >> 3) & 0xffu,
         (unsigned)(96u + (frame_count % 96u)));
   nav_bar = (unsigned)((frame_count / 8u) % DEEVEE_VIDEO_WIDTH);

   for (y = 0; y < DEEVEE_VIDEO_HEIGHT; y++)
   {
      for (x = 0; x < DEEVEE_VIDEO_WIDTH; x++)
      {
         uint32_t color;
         bool guide_line = (x % 120u) == 0 || (y % 80u) == 0;
         bool moving_bar = x >= nav_bar && x < nav_bar + 8u;
         bool title_safe = x > 64u && x < DEEVEE_VIDEO_WIDTH - 64u &&
            y > 44u && y < DEEVEE_VIDEO_HEIGHT - 44u;

         if (moving_bar)
            color = accent;
         else if (guide_line)
            color = deevee_rgb(36, 58, 78);
         else if (title_safe)
            color = deevee_rgb(12 + (x / 32u), 18 + (y / 32u), 34);
         else
            color = deevee_rgb(6, 8, 14);

         video->pixels[y * DEEVEE_VIDEO_WIDTH + x] = color;
      }
   }
}

void deevee_video_render_decoded_overlay(struct deevee_video *video,
      uint64_t frame_count)
{
   unsigned x;
   unsigned y;
   unsigned marker_x;
   uint32_t green = deevee_rgb(32, 255, 96);
   uint32_t dark_green = deevee_rgb(0, 96, 32);

   if (!video || !video->pixels)
      return;

   marker_x = (unsigned)((frame_count * 6u) % DEEVEE_VIDEO_WIDTH);

   for (y = 0; y < DEEVEE_VIDEO_HEIGHT; y++)
   {
      for (x = 0; x < DEEVEE_VIDEO_WIDTH; x++)
      {
         bool border = x < 6u || y < 6u ||
            x >= DEEVEE_VIDEO_WIDTH - 6u ||
            y >= DEEVEE_VIDEO_HEIGHT - 6u;
         bool corner_block =
            (x < 54u && y < 34u) ||
            (x >= DEEVEE_VIDEO_WIDTH - 54u && y < 34u) ||
            (x < 54u && y >= DEEVEE_VIDEO_HEIGHT - 34u) ||
            (x >= DEEVEE_VIDEO_WIDTH - 54u &&
               y >= DEEVEE_VIDEO_HEIGHT - 34u);
         bool marker = y >= 14u && y < 24u &&
            x >= marker_x && x < marker_x + 28u;

         if (border || corner_block || marker)
            video->pixels[y * DEEVEE_VIDEO_WIDTH + x] =
               marker ? green : dark_green;
      }
   }
}
