/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libdvdnav open sequence + event loop (CLAUDE.md §5.2).
 */
#ifndef DV_NAV_H
#define DV_NAV_H

#include <stdbool.h>
#include <stdint.h>
#include <dvdnav/dvdnav.h>

#include "dv_source.h"

struct dv_ctx;

typedef struct dv_nav
{
   dvdnav_t   *dvdnav;
   dv_stream_t stream;        /* backing handle when the source is an image */

   int32_t disc_region_mask;  /* bit n set = region n+1 allowed */
   int32_t vm_region;         /* 1..8 chosen for the VM */
   uint8_t ifo_aspect;        /* dvdnav_get_video_aspect(): 0 = 4:3, 3 = 16:9 */

   /* Still frame (STILL_FRAME event). */
   bool     still;
   bool     still_infinite;
   int      still_ticks_left; /* -1: waiting for video queue to drain */
   unsigned still_length;     /* seconds */

   /* WAIT event: hold reading until queued video is presented. */
   bool     wait;

   /* VOBU still flush (VLC dvdnav.c i_vobu_index / i_vobu_flush). */
   uint32_t vobu_index;
   uint32_t vobu_flush;

   bool     stopped;          /* DVDNAV_STOP seen */
   uint32_t blocks;           /* BLOCK_OK packs received (stats) */
   uint32_t stills, waits, hops, discontinuities;
} dv_nav_t;

bool dv_nav_open(struct dv_ctx *ctx, const dv_source_t *src);
void dv_nav_close(struct dv_ctx *ctx);

/* Log disc-level info: volume ID, title string, region mask, title/VTS count. */
void dv_nav_log_disc_info(struct dv_ctx *ctx, const dv_source_t *src);

/* Read and dispatch blocks until the video queue reaches past the clock, a
 * still/wait/stop holds reading, or max_blocks is spent. */
void dv_nav_fill(struct dv_ctx *ctx, int max_blocks);

/* Per-tick still timer and WAIT release; call after presenting video. */
void dv_nav_service(struct dv_ctx *ctx);

/* User action (temporary M1 binding for A): activate the highlighted menu
 * button, else skip a still. Full input mapping is M4. */
void dv_nav_activate(struct dv_ctx *ctx);

#endif
