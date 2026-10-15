/*
 * DeeVee — headless libretro test frontend (CLAUDE.md §8)
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Loads a core, loads content, runs N frames, unloads. Prints log output and
 * a running hash of video/audio so later milestones can do regression checks.
 *
 *   harness <core.so> <content> [frames] [system_dir]
 *
 * TODO(M4): scripted input injection; frame dumps.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

static const char *g_sysdir = ".";
static uint64_t g_video_hash = 1469598103934665603ULL;
static uint64_t g_audio_frames;
static unsigned g_video_frames, g_dupes;
static unsigned g_width, g_height;

static void h_log(enum retro_log_level level, const char *fmt, ...)
{
   static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
   va_list va;
   if (level < RETRO_LOG_DEBUG && !getenv("HARNESS_DEBUG"))
      return;
   fprintf(stderr, "[%s] ", names[level <= RETRO_LOG_ERROR ? level : 0]);
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

static bool h_env(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = h_log;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char **)data = g_sysdir;
         return true;
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
         return true;
      default:
         return false;  /* incl. VFS: exercises the stdio fallback */
   }
}

static void h_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
   unsigned y;
   g_video_frames++;
   if (!data)
   {
      g_dupes++;
      return;
   }
   g_width = w;
   g_height = h;
   for (y = 0; y < h; y++)
   {
      const uint8_t *row = (const uint8_t *)data + y * pitch;
      size_t i;
      for (i = 0; i < (size_t)w * 4; i++)
         g_video_hash = (g_video_hash ^ row[i]) * 1099511628211ULL;
   }
}

static size_t h_audio_batch(const int16_t *data, size_t frames)
{
   g_audio_frames += frames;
   return frames;
}

static void h_audio(int16_t l, int16_t r) { g_audio_frames++; }
static void h_poll(void) { }
static int16_t h_input(unsigned port, unsigned dev, unsigned idx, unsigned id) { return 0; }

#define LOAD(sym) do { \
      *(void **)&p_##sym = dlsym(lib, #sym); \
      if (!p_##sym) { fprintf(stderr, "missing symbol %s\n", #sym); return 1; } \
   } while (0)

int main(int argc, char **argv)
{
   void *lib;
   unsigned frames = argc > 3 ? (unsigned)atoi(argv[3]) : 120, i;
   struct retro_system_info sys;
   struct retro_system_av_info av;
   struct retro_game_info game = { 0 };

   void (*p_retro_set_environment)(retro_environment_t);
   void (*p_retro_set_video_refresh)(retro_video_refresh_t);
   void (*p_retro_set_audio_sample)(retro_audio_sample_t);
   void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*p_retro_set_input_poll)(retro_input_poll_t);
   void (*p_retro_set_input_state)(retro_input_state_t);
   void (*p_retro_init)(void);
   void (*p_retro_deinit)(void);
   void (*p_retro_get_system_info)(struct retro_system_info *);
   void (*p_retro_get_system_av_info)(struct retro_system_av_info *);
   bool (*p_retro_load_game)(const struct retro_game_info *);
   void (*p_retro_unload_game)(void);
   void (*p_retro_run)(void);

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s <core> <content> [frames] [system_dir]\n", argv[0]);
      return 2;
   }
   if (argc > 4)
      g_sysdir = argv[4];

   lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib)
   {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      return 1;
   }
   LOAD(retro_set_environment);  LOAD(retro_set_video_refresh);
   LOAD(retro_set_audio_sample); LOAD(retro_set_audio_sample_batch);
   LOAD(retro_set_input_poll);   LOAD(retro_set_input_state);
   LOAD(retro_init);             LOAD(retro_deinit);
   LOAD(retro_get_system_info);  LOAD(retro_get_system_av_info);
   LOAD(retro_load_game);        LOAD(retro_unload_game);
   LOAD(retro_run);

   p_retro_get_system_info(&sys);
   printf("core: %s %s\n", sys.library_name, sys.library_version);

   p_retro_set_environment(h_env);
   p_retro_init();
   p_retro_set_video_refresh(h_video);
   p_retro_set_audio_sample(h_audio);
   p_retro_set_audio_sample_batch(h_audio_batch);
   p_retro_set_input_poll(h_poll);
   p_retro_set_input_state(h_input);

   game.path = argv[2];
   if (!p_retro_load_game(&game))
   {
      fprintf(stderr, "retro_load_game failed\n");
      p_retro_deinit();
      dlclose(lib);
      return 1;
   }
   p_retro_get_system_av_info(&av);
   printf("av: %ux%u (max %ux%u) aspect %.4f fps %.4f rate %.0f\n",
          av.geometry.base_width, av.geometry.base_height,
          av.geometry.max_width, av.geometry.max_height,
          av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

   for (i = 0; i < frames; i++)
      p_retro_run();

   p_retro_unload_game();
   p_retro_deinit();
   dlclose(lib);

   printf("frames: %u (dupes %u) last %ux%u video_hash %016llx audio_frames %llu (expected %.0f)\n",
          g_video_frames, g_dupes, g_width, g_height,
          (unsigned long long)g_video_hash, (unsigned long long)g_audio_frames,
          frames * av.timing.sample_rate / av.timing.fps);
   return 0;
}
