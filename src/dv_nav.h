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

   bool     stopped;          /* DVDNAV_STOP seen */
   uint32_t blocks;           /* BLOCK_OK packs received (stats) */
} dv_nav_t;

bool dv_nav_open(struct dv_ctx *ctx, const dv_source_t *src);
void dv_nav_close(struct dv_ctx *ctx);

/* Log disc-level info: volume ID, title string, region mask, title/VTS count. */
void dv_nav_log_disc_info(struct dv_ctx *ctx, const dv_source_t *src);

/* Pull and dispatch up to max_blocks packs/events. M0: events are logged and
 * stills/waits are skipped; later milestones route packs and act on events. */
void dv_nav_pump(struct dv_ctx *ctx, int max_blocks);

#endif
