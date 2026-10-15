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
 * Environment:
 *   HARNESS_DEBUG=1                 show DEBUG-level core logs
 *   HARNESS_INPUT="300:A,900:START" press a RetroPad button on a frame (held
 *                                   for 2 frames); names: A B X Y L R L2 R2
 *                                   L3 R3 START SELECT UP DOWN LEFT RIGHT
 *   HARNESS_DUMP=<dir>              write PPM snapshots there...
 *   HARNESS_DUMP_FRAMES="60,600"    ...at these frame numbers (default: 60)
 *   HARNESS_DUMP_EVERY=<n>          ...or every n frames
 *   HARNESS_AVSYNC=1                measure A/V offset on the avs_* fixtures:
 *                                   white-flash onsets (video) vs beep onsets
 *                                   (audio), timed on the audio sample clock
 *   HARNESS_WAV=<file>              write the audio output as a WAV
 *   HARNESS_POINTER="300:400,350,600:400,350:press"
 *                                   pointer at frame:x,y (stored-frame
 *                                   pixels, 720x480); ":press" holds the
 *                                   button for 2 frames
 *   HARNESS_OPT_<key>=<value>       core option value, e.g.
 *                                   HARNESS_OPT_deevee_subtitles=on
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

static const char *g_sysdir = ".";
static unsigned g_frame_no;
static uint64_t g_video_hash = 1469598103934665603ULL;
static uint64_t g_audio_frames;
static unsigned g_video_frames, g_dupes;
static unsigned g_width, g_height;
static unsigned g_geom_changes, g_av_changes;

/* last non-dupe frame, for dumps */
static uint32_t *g_last;
static size_t    g_last_pitch;

/* A/V sync analysis */
#define MAX_SYNC 4096
static double   g_vflash[MAX_SYNC], g_abeep[MAX_SYNC];
static unsigned g_nvflash, g_nabeep;
static bool     g_flash_on;
static uint64_t g_quiet_run = 1000000;
static FILE    *g_wav;
static int16_t  g_prev_l, g_prev_r;
static unsigned g_clicks, g_max_step;
static double   g_first_click = -1;

static void avsync_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
   /* Mean of a 5x5 grid of pixels: flash frames are white, others black. */
   unsigned x, y, sum = 0;
   bool on;
   if (!getenv("HARNESS_AVSYNC"))
      return;
   if (data)
   {
      for (y = 1; y <= 5; y++)
         for (x = 1; x <= 5; x++)
         {
            uint32_t p = ((const uint32_t *)((const uint8_t *)data + (h * y / 6) * pitch))[w * x / 6];
            sum += (p >> 8) & 0xFF;
         }
      on = sum / 25 > 128;
      if (on && !g_flash_on && g_nvflash < MAX_SYNC)
         g_vflash[g_nvflash++] = g_audio_frames / 48000.0;
      g_flash_on = on;
   }
}

static void avsync_audio(const int16_t *data, size_t frames)
{
   size_t i;
   if (!getenv("HARNESS_AVSYNC"))
      return;
   for (i = 0; i < frames; i++)
   {
      int amp = abs(data[2 * i]) + abs(data[2 * i + 1]);
      /* Clicks: a 1 kHz tone at amplitude A moves <= ~0.13 A per sample at
       * 48 kHz, so a bigger step is a waveform discontinuity. */
      int step = abs(data[2 * i] - g_prev_l) + abs(data[2 * i + 1] - g_prev_r);
      if ((unsigned)step > g_max_step)
         g_max_step = (unsigned)step;
      if (step > 2500)
      {
         if (g_first_click < 0)
            g_first_click = (g_audio_frames + i) / 48000.0;
         if (atoi(getenv("HARNESS_AVSYNC")) >= 2)
            printf("  click at %.4f s (step %d)\n", (g_audio_frames + i) / 48000.0, step);
         g_clicks++;
      }
      g_prev_l = data[2 * i];
      g_prev_r = data[2 * i + 1];
      if (amp > 1500)
      {
         /* onset = loud after >= 200 ms of quiet */
         if (g_quiet_run >= 9600 && g_nabeep < MAX_SYNC)
            g_abeep[g_nabeep++] = (g_audio_frames + i) / 48000.0;
         g_quiet_run = 0;
      }
      else
         g_quiet_run++;
   }
}

static void avsync_report(void)
{
   unsigned i, j, matched = 0;
   double sum = 0, mn = 1e9, mx = -1e9;
   if (!getenv("HARNESS_AVSYNC"))
      return;
   for (i = 0; i < g_nvflash; i++)
   {
      double best = 1e9;
      for (j = 0; j < g_nabeep; j++)
      {
         double d = g_abeep[j] - g_vflash[i];
         if (d * d < best * best)
            best = d;
      }
      if (best > -0.5 && best < 0.5)
      {
         if (atoi(getenv("HARNESS_AVSYNC")) >= 2)
            printf("  flash %8.4f s  offset %+6.1f ms\n", g_vflash[i], 1000 * best);
         matched++;
         sum += best;
         if (best < mn) mn = best;
         if (best > mx) mx = best;
      }
   }
   printf("avsync: flashes %u, beeps %u, matched %u; audio-video offset ms: mean %+.1f min %+.1f max %+.1f\n",
          g_nvflash, g_nabeep, matched, matched ? 1000 * sum / matched : 0,
          matched ? 1000 * mn : 0, matched ? 1000 * mx : 0);
   printf("avsync: clicks %u (max step %u, first at %.3f s)\n", g_clicks, g_max_step, g_first_click);
}

static void wav_open(const char *path)
{
   static const uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
      'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0,
      'd','a','t','a', 0,0,0,0 };
   if (!path || !(g_wav = fopen(path, "wb")))
      return;
   fwrite(hdr, 1, sizeof(hdr), g_wav);
}

static void wav_close(void)
{
   long size;
   uint8_t b[4];
   if (!g_wav)
      return;
   size = ftell(g_wav);
   fseek(g_wav, 4, SEEK_SET);
   b[0] = (uint8_t)(size - 8); b[1] = (uint8_t)((size - 8) >> 8);
   b[2] = (uint8_t)((size - 8) >> 16); b[3] = (uint8_t)((size - 8) >> 24);
   fwrite(b, 1, 4, g_wav);
   fseek(g_wav, 40, SEEK_SET);
   b[0] = (uint8_t)(size - 44); b[1] = (uint8_t)((size - 44) >> 8);
   b[2] = (uint8_t)((size - 44) >> 16); b[3] = (uint8_t)((size - 44) >> 24);
   fwrite(b, 1, 4, g_wav);
   fclose(g_wav);
}

#define MAX_EVENTS 256
static struct { unsigned frame; unsigned id; } g_input[MAX_EVENTS];
static unsigned g_ninput;

static const struct { const char *name; unsigned id; } g_buttons[] = {
   { "A", RETRO_DEVICE_ID_JOYPAD_A }, { "B", RETRO_DEVICE_ID_JOYPAD_B },
   { "X", RETRO_DEVICE_ID_JOYPAD_X }, { "Y", RETRO_DEVICE_ID_JOYPAD_Y },
   { "L", RETRO_DEVICE_ID_JOYPAD_L }, { "R", RETRO_DEVICE_ID_JOYPAD_R },
   { "L2", RETRO_DEVICE_ID_JOYPAD_L2 }, { "R2", RETRO_DEVICE_ID_JOYPAD_R2 },
   { "L3", RETRO_DEVICE_ID_JOYPAD_L3 }, { "R3", RETRO_DEVICE_ID_JOYPAD_R3 },
   { "START", RETRO_DEVICE_ID_JOYPAD_START }, { "SELECT", RETRO_DEVICE_ID_JOYPAD_SELECT },
   { "UP", RETRO_DEVICE_ID_JOYPAD_UP }, { "DOWN", RETRO_DEVICE_ID_JOYPAD_DOWN },
   { "LEFT", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "RIGHT", RETRO_DEVICE_ID_JOYPAD_RIGHT },
};

static struct { unsigned frame; int x, y; bool press; } g_ptr[MAX_EVENTS];
static unsigned g_nptr;

static void parse_pointer(const char *spec)
{
   char buf[4096], *tok, *save = NULL;
   if (!spec)
      return;
   snprintf(buf, sizeof(buf), "%s", spec);
   for (tok = strtok_r(buf, ",", &save); tok && g_nptr < MAX_EVENTS;
        tok = strtok_r(NULL, ",", &save))
   {
      /* tokens come in pairs: "frame:x" then "y[:press]" */
      char *y = strtok_r(NULL, ",", &save);
      char *colon = strchr(tok, ':');
      if (!colon || !y)
         break;
      g_ptr[g_nptr].frame = (unsigned)atoi(tok);
      g_ptr[g_nptr].x     = atoi(colon + 1);
      g_ptr[g_nptr].y     = atoi(y);
      g_ptr[g_nptr].press = strstr(y, "press") != NULL;
      g_nptr++;
   }
}

static int16_t pointer_state(unsigned id)
{
   int i, cur = -1;
   for (i = 0; i < (int)g_nptr; i++)
      if (g_ptr[i].frame <= g_frame_no)
         cur = i;
   if (cur < 0)
      return 0;
   switch (id)
   {
      case RETRO_DEVICE_ID_POINTER_X:
         return (int16_t)(g_ptr[cur].x * 0xfffe / 720 - 0x7fff);
      case RETRO_DEVICE_ID_POINTER_Y:
         return (int16_t)(g_ptr[cur].y * 0xfffe / 480 - 0x7fff);
      case RETRO_DEVICE_ID_POINTER_PRESSED:
         return g_ptr[cur].press && g_frame_no < g_ptr[cur].frame + 2;
   }
   return 0;
}

static void parse_input(const char *spec)
{
   char buf[4096], *tok, *save = NULL;
   if (!spec)
      return;
   snprintf(buf, sizeof(buf), "%s", spec);
   for (tok = strtok_r(buf, ",", &save); tok && g_ninput < MAX_EVENTS;
        tok = strtok_r(NULL, ",", &save))
   {
      char *colon = strchr(tok, ':');
      size_t i;
      if (!colon)
         continue;
      *colon = '\0';
      for (i = 0; i < sizeof(g_buttons) / sizeof(g_buttons[0]); i++)
         if (!strcmp(colon + 1, g_buttons[i].name))
         {
            g_input[g_ninput].frame = (unsigned)atoi(tok);
            g_input[g_ninput].id    = g_buttons[i].id;
            g_ninput++;
         }
   }
}

static bool want_dump(unsigned frame)
{
   const char *every = getenv("HARNESS_DUMP_EVERY");
   const char *list  = getenv("HARNESS_DUMP_FRAMES");
   char buf[4096], *tok, *save = NULL;

   if (!getenv("HARNESS_DUMP"))
      return false;
   if (every && atoi(every) > 0)
      return frame % (unsigned)atoi(every) == 0;
   if (!list)
      return frame == 60;
   snprintf(buf, sizeof(buf), "%s", list);
   for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
      if ((unsigned)atoi(tok) == frame)
         return true;
   return false;
}

static void dump_frame(unsigned frame)
{
   char path[4096];
   FILE *f;
   unsigned x, y;

   if (!g_last)
      return;
   snprintf(path, sizeof(path), "%s/frame_%06u.ppm", getenv("HARNESS_DUMP"), frame);
   f = fopen(path, "wb");
   if (!f)
      return;
   fprintf(f, "P6\n%u %u\n255\n", g_width, g_height);
   for (y = 0; y < g_height; y++)
   {
      const uint32_t *row = (const uint32_t *)((const uint8_t *)g_last + y * g_last_pitch);
      for (x = 0; x < g_width; x++)
      {
         uint8_t rgb[3] = { (uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x] };
         fwrite(rgb, 1, 3, f);
      }
   }
   fclose(f);
}

static void h_log(enum retro_log_level level, const char *fmt, ...)
{
   static const char *names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
   va_list va;
   if (level < RETRO_LOG_DEBUG && !getenv("HARNESS_DEBUG"))
      return;
   fprintf(stderr, "[f%u %s] ", g_frame_no, names[level <= RETRO_LOG_ERROR ? level : 0]);
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
      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned *)data = 2;
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = data;
         char name[256];
         snprintf(name, sizeof(name), "HARNESS_OPT_%s", var->key);
         var->value = getenv(name);
         return var->value != NULL;
      }
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true;
         return true;
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      {
         const struct retro_game_geometry *g = data;
         g_geom_changes++;
         printf("frame %u: geometry %ux%u aspect %.4f\n", g_frame_no,
                g->base_width, g->base_height, g->aspect_ratio);
         return true;
      }
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      {
         const struct retro_system_av_info *av = data;
         g_av_changes++;
         printf("frame %u: av_info %ux%u aspect %.4f fps %.3f\n", g_frame_no,
                av->geometry.base_width, av->geometry.base_height,
                av->geometry.aspect_ratio, av->timing.fps);
         return true;
      }
      default:
         return false;  /* incl. VFS: exercises the stdio fallback */
   }
}

static void h_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
   unsigned y;
   g_video_frames++;
   avsync_video(data, w, h, pitch);
   if (!data)
   {
      g_dupes++;
      return;
   }
   g_width = w;
   g_height = h;
   if (getenv("HARNESS_DUMP"))
   {
      size_t sz = pitch * h;
      g_last = realloc(g_last, sz);
      if (g_last)
         memcpy(g_last, data, sz);
      g_last_pitch = pitch;
   }
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
   avsync_audio(data, frames);
   if (g_wav)
      fwrite(data, sizeof(int16_t) * 2, frames, g_wav);
   g_audio_frames += frames;
   return frames;
}

static void h_audio(int16_t l, int16_t r) { (void)l; (void)r; g_audio_frames++; }
static void h_poll(void) { }
static int16_t h_input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
   unsigned i;
   if (port == 0 && dev == RETRO_DEVICE_POINTER && idx == 0)
      return pointer_state(id);
   if (port != 0 || dev != RETRO_DEVICE_JOYPAD)
      return 0;
   for (i = 0; i < g_ninput; i++)
      if (g_input[i].id == id && g_frame_no >= g_input[i].frame &&
          g_frame_no < g_input[i].frame + 2)
         return 1;
   return 0;
}

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

   parse_input(getenv("HARNESS_INPUT"));
   parse_pointer(getenv("HARNESS_POINTER"));
   wav_open(getenv("HARNESS_WAV"));
   for (i = 0; i < frames; i++)
   {
      g_frame_no = i;
      p_retro_run();
      if (want_dump(i))
         dump_frame(i);
   }

   p_retro_unload_game();
   p_retro_deinit();
   dlclose(lib);

   wav_close();
   avsync_report();
   printf("geometry changes %u, av_info changes %u\n", g_geom_changes, g_av_changes);
   printf("frames: %u (dupes %u) last %ux%u video_hash %016llx audio_frames %llu (expected %.0f)\n",
          g_video_frames, g_dupes, g_width, g_height,
          (unsigned long long)g_video_hash, (unsigned long long)g_audio_frames,
          frames * av.timing.sample_rate / av.timing.fps);
   return 0;
}
