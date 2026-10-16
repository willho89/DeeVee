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

#include <dvdread/nav_types.h>

#include "dv_source.h"

struct dv_ctx;

/* PCIs are read up to ~3 s ahead of presentation; keep them with their
 * timeline start so highlights and button navigation use the one on screen. */
#define DV_PCI_QUEUE 32

typedef struct dv_pci_slot
{
   int64_t start;      /* timeline of vobu_s_ptm */
   pci_t   pci;
} dv_pci_slot_t;

typedef enum
{
   DV_BTN_UP, DV_BTN_DOWN, DV_BTN_LEFT, DV_BTN_RIGHT
} dv_button_dir_t;

typedef enum
{
   DV_ACT_ACTIVATE,     /* A: activate button / skip still */
   DV_ACT_GO_UP,        /* B: Return */
   DV_ACT_ROOT_MENU,    /* Start */
   DV_ACT_TITLE_MENU,   /* Select */
   DV_ACT_PREV_CHAPTER, /* L */
   DV_ACT_NEXT_CHAPTER  /* R */
} dv_action_t;

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

   int32_t  policy_title;     /* title the subtitle options were last applied to */

   dv_pci_slot_t pcis[DV_PCI_QUEUE];
   int      pci_head, pci_count;

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

/* Apply the per-title core options (audio language, subtitle language and
 * on/off) to the current title. Runs once per title unless force is set
 * (an option changed), so the user's later choices aren't overridden. */
void dv_nav_apply_title_options(struct dv_ctx *ctx, bool force);

/* PCI of the VOBU on screen (NULL if none yet). */
const pci_t *dv_nav_presented_pci(struct dv_ctx *ctx);

/* True when the presented VOBU has buttons (menu-style input applies). */
bool dv_nav_has_buttons(struct dv_ctx *ctx);

/* Recompute the menu highlight (port of VLC ButtonUpdate()); call per tick. */
void dv_nav_update_highlight(struct dv_ctx *ctx);

/* User input (CLAUDE.md §5.9). */
void dv_nav_button_move(struct dv_ctx *ctx, dv_button_dir_t dir);
void dv_nav_action(struct dv_ctx *ctx, dv_action_t act);
/* Pointer in stored-frame coordinates (720 x 480/576). */
void dv_nav_pointer(struct dv_ctx *ctx, int x, int y, bool activate);

#endif
