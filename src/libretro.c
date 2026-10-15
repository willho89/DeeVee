/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libretro entry points (CLAUDE.md §3). The only file allowed globals.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "dv_ctx.h"

#define DV_BASE_WIDTH   720
#define DV_BASE_HEIGHT  480
#define DV_MAX_WIDTH    720
#define DV_MAX_HEIGHT   576
#define DV_SAMPLE_RATE  48000

/* Packs pulled per retro_run while there is no decoder to pace against.
 * 16 x 2 KiB x 59.94 Hz ≈ 1.9 MiB/s — roughly a DVD's peak mux rate. */
#define DV_M0_BLOCKS_PER_FRAME 16

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static dv_ctx_t *g_ctx;

static uint32_t g_frame[DV_MAX_WIDTH * DV_MAX_HEIGHT];
static int16_t  g_silence[2 * 2048];
static double   g_fps = 60000.0 / 1001.0;
static double   g_audio_accum;

/* ---- logging ---------------------------------------------------------- */

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void dv_log(dv_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...)
{
   char buf[2048];
   va_list va;
   retro_log_printf_t cb = (ctx && ctx->log_cb) ? ctx->log_cb : (log_cb ? log_cb : fallback_log);

   va_start(va, fmt);
   vsnprintf(buf, sizeof(buf), fmt, va);
   va_end(va);
   cb(level, "[DeeVee] %s", buf);
}

/* ---- libretro API ----------------------------------------------------- */

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool no_game = false;

   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;

   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "DeeVee";
   info->library_version  = DV_VERSION " (css:" DEEVEE_CSS_MODE ")";
   info->valid_extensions = "iso|ifo|img";
   info->need_fullpath    = true;
   info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   /* TODO(M1): geometry/timing from the VTS video attributes. */
   info->geometry.base_width   = DV_BASE_WIDTH;
   info->geometry.base_height  = DV_BASE_HEIGHT;
   info->geometry.max_width    = DV_MAX_WIDTH;
   info->geometry.max_height   = DV_MAX_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = g_fps;
   info->timing.sample_rate    = DV_SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

RETRO_API void retro_reset(void)
{
   /* TODO(M4): dvdnav_menu_call(DVD_MENU_Root) / restart First Play PGC. */
}

static void output_silence(void)
{
   /* Fractional accumulation so 48000/59.94 = 800.8 averages out exactly. */
   size_t frames;
   g_audio_accum += DV_SAMPLE_RATE / g_fps;
   frames = (size_t)g_audio_accum;
   g_audio_accum -= (double)frames;

   while (frames)
   {
      size_t n = frames > 2048 ? 2048 : frames;
      audio_batch_cb(g_silence, n);
      frames -= n;
   }
}

RETRO_API void retro_run(void)
{
   input_poll_cb();

   if (g_ctx)
      dv_nav_pump(g_ctx, DV_M0_BLOCKS_PER_FRAME);

   video_cb(g_frame, DV_BASE_WIDTH, DV_BASE_HEIGHT, DV_MAX_WIDTH * sizeof(uint32_t));
   output_silence();
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_vfs_interface_info vfs_info = { 1, NULL };
   const char *sysdir = NULL;

   if (!game || !game->path)
      return false;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      dv_log(NULL, RETRO_LOG_ERROR, "XRGB8888 is not supported by the frontend.\n");
      return false;
   }

   g_ctx = calloc(1, sizeof(*g_ctx));
   if (!g_ctx)
      return false;
   g_ctx->environ_cb = environ_cb;
   g_ctx->log_cb     = log_cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info) && vfs_info.iface)
      g_ctx->vfs = vfs_info.iface;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) && sysdir)
      snprintf(g_ctx->system_dir, sizeof(g_ctx->system_dir), "%s", sysdir);

   dv_log(g_ctx, RETRO_LOG_INFO, "DeeVee %s (css:%s), VFS %s\n", DV_VERSION,
          DEEVEE_CSS_MODE, g_ctx->vfs ? "available" : "unavailable (stdio fallback)");

   dv_source_setup_css_env(g_ctx);

   if (!dv_source_resolve(g_ctx, game->path, &g_ctx->source) ||
       !dv_nav_open(g_ctx, &g_ctx->source))
   {
      free(g_ctx);
      g_ctx = NULL;
      return false;
   }

   dv_nav_log_disc_info(g_ctx, &g_ctx->source);

   memset(g_frame, 0, sizeof(g_frame));
   g_audio_accum = 0.0;
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}

RETRO_API void retro_unload_game(void)
{
   if (!g_ctx)
      return;
   dv_log(g_ctx, RETRO_LOG_INFO, "Unloading (%u packs read).\n", g_ctx->nav.blocks);
   dv_nav_close(g_ctx);
   free(g_ctx);
   g_ctx = NULL;
}

RETRO_API unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

/* TODO(M7): resume-point save states (§5.12). */
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { return false; }
RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { }
RETRO_API void *retro_get_memory_data(unsigned id) { return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
