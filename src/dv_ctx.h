/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared core context. No globals outside libretro.c: every module takes a
 * dv_ctx_t* (CLAUDE.md §10).
 */
#ifndef DV_CTX_H
#define DV_CTX_H

#include <stdbool.h>
#include <stdint.h>
#include <libretro.h>

#include "dv_source.h"
#include "dv_nav.h"
#include "dv_clock.h"
#include "dv_ps.h"
#include "dv_video.h"
#include "dv_render.h"
#include "dv_audio.h"
#include "dv_spu.h"
#include "dv_overlay.h"
#include "dv_input.h"

#define DV_VERSION "0.5.0"

#ifndef DEEVEE_CSS_MODE
#define DEEVEE_CSS_MODE "unknown"
#endif

typedef enum
{
   DV_SUBS_AUTO = 0,   /* follow the disc */
   DV_SUBS_ON,
   DV_SUBS_OFF         /* forced subtitles still show */
} dv_subs_mode_t;

/* TV Display Type option (M6b, D-025): the TV the virtual player drives. */
typedef enum
{
   DV_TV_WIDE = 0,     /* 16:9 */
   DV_TV_LETTERBOX,    /* 4:3, 16:9 content letterboxed */
   DV_TV_PANSCAN       /* 4:3, 16:9 content panned & scanned */
} dv_tv_t;

/* Core option values (CLAUDE.md §5.10), read by libretro.c. */
typedef struct dv_options
{
   dv_subs_mode_t subtitles;
   char           sub_lang[3];   /* ISO 639-1, "" = disc default */
   char           audio_lang[3];
   char           menu_lang[3];
   int            region;        /* 0 = auto (from disc), else 1..8 */
   bool           osd;           /* on-screen messages */
   int            deint;         /* dv_deint_t */
   int            aspect;        /* 0 auto, 1 = 4:3, 2 = 16:9 */
   int            tv_display;    /* dv_tv_t */
   bool           dplii;         /* Pro Logic II downmix instead of plain stereo */
   int            drc;           /* 0 normal (line), 1 off, 2 night (heavy) */
   int            end_of_disc;   /* 0 restart, 1 stop, 2 close content */
   char           css_method[8]; /* "" = libdvdcss default, key|disc|title */
} dv_options_t;

typedef struct dv_ctx
{
   retro_environment_t     environ_cb;
   retro_log_printf_t      log_cb;
   struct retro_vfs_interface *vfs;   /* NULL if frontend has no VFS v1+ */

   char system_dir[4096];

   dv_source_t   source;
   dv_nav_t      nav;
   dv_clock_t    clock;
   dv_ps_stats_t ps_stats;
   dv_video_t    video;
   dv_render_t   render;
   dv_audio_t    audio;
   dv_spu_t      spu;
   dv_overlay_t  overlay;
   dv_highlight_t highlight;    /* current menu button highlight */
   dv_input_t    input;
   bool          paused;        /* user pause: clock, reading, audio stopped */
   dv_options_t  opt;
} dv_ctx_t;

void dv_log(dv_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...)
#ifdef __GNUC__
   __attribute__((format(printf, 3, 4)))
#endif
   ;

/* On-screen message (CLAUDE.md §5.11), honours the Show OSD option. */
void dv_osd(dv_ctx_t *ctx, unsigned ms, const char *fmt, ...)
#ifdef __GNUC__
   __attribute__((format(printf, 3, 4)))
#endif
   ;

#endif
