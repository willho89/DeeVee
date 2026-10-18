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
   int64_t ptime;      /* PGC time of this VOBU (90 kHz, dvdnav_get_current_time) */
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
   DV_ACT_NEXT_CHAPTER, /* R */
   DV_ACT_CYCLE_AUDIO,  /* X */
   DV_ACT_CYCLE_SUBS,   /* Y */
   DV_ACT_CYCLE_ANGLE,  /* L3 */
   DV_ACT_INFO          /* Select during a title */
} dv_action_t;

typedef struct dv_nav
{
   dvdnav_t   *dvdnav;
   dv_stream_t stream;        /* backing handle when the source is an image */

   int32_t disc_region_mask;  /* bit n set = region n+1 allowed */
   int32_t vm_region;         /* 1..8 chosen for the VM */
   uint8_t ifo_aspect;        /* dvdnav_get_video_aspect(): 0 = 4:3, 3 = 16:9 */
   uint8_t disc_id[16];       /* libdvdread DVDDiscID (MD5 of the IFOs) */
   bool    have_disc_id;
   uint8_t permitted_df;      /* IFO: 0 LB+P&S, 1 P&S only, 2 LB only, 3 none */
   bool    spu_native;        /* selected SPU is a letterbox/pan&scan variant,
                               * authored for the 4:3 output frame */

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
   int64_t  pgc_length;       /* 90 kHz, from CELL_CHANGE */
   int32_t  last_title, last_part;  /* for chapter OSD */
   int64_t  seek_target;      /* last seek target, -1 = none pending (logs landing) */

   uint32_t logged_btn_lbn;   /* debug: last PCI whose buttons were logged */
   btni_t   logged_btnit[36];

   bool     stopped;          /* DVDNAV_STOP seen */
   int      read_errors;      /* consecutive get_next_block errors (damaged VOBU skip) */
   uint32_t blocks;           /* BLOCK_OK packs received (stats) */
   uint32_t stills, waits, hops, discontinuities;
} dv_nav_t;

bool dv_nav_open(struct dv_ctx *ctx, const dv_source_t *src);

/* Resume points as save states (D-031): a fixed-size blob holding the
 * libdvdnav VM state (position of the picture on screen + disc registers),
 * tied to the disc by its libdvdread disc ID. */
#define DV_STATE_SIZE 512
size_t dv_nav_state_size(void);
bool   dv_nav_save_state(struct dv_ctx *ctx, uint8_t *buf, size_t size);
bool   dv_nav_load_state(struct dv_ctx *ctx, const uint8_t *buf, size_t size);
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

/* Physical SPU stream for the current display mode (dv_spu_select value);
 * also updates nav.spu_native. */
int dv_nav_active_spu(struct dv_ctx *ctx);

/* Apply a changed TV Display Type: SPRM 14 + SPU variant. */
void dv_nav_apply_display(struct dv_ctx *ctx);

/* True if overlays are drawn on the 4:3 output frame rather than the
 * stored picture (a letterbox/pan&scan SPU variant is on screen). */
bool dv_nav_overlay_native(struct dv_ctx *ctx);

/* PCI of the VOBU on screen (NULL if none yet). */
const pci_t *dv_nav_presented_pci(struct dv_ctx *ctx);

/* True when the presented VOBU has buttons (menu-style input applies). */
bool dv_nav_has_buttons(struct dv_ctx *ctx);

/* Recompute the menu highlight (port of VLC ButtonUpdate()); call per tick. */
void dv_nav_update_highlight(struct dv_ctx *ctx);

/* User input (CLAUDE.md §5.9). */
void dv_nav_button_move(struct dv_ctx *ctx, dv_button_dir_t dir);
void dv_nav_action(struct dv_ctx *ctx, dv_action_t act);
/* Start the disc again from First Play (end-of-disc 'restart'). */
bool dv_nav_restart(struct dv_ctx *ctx);

/* Title time on screen (90 kHz, PGC-relative), -1 if unknown. */
int64_t dv_nav_presented_time(struct dv_ctx *ctx);

/* Seek within the current title by delta (90 kHz); VOBU-accurate via the
 * title's time map (dvdnav_jump_to_sector_by_time). */
void dv_nav_seek(struct dv_ctx *ctx, int64_t delta);

/* True while playing a title without buttons (title controls apply). */
bool dv_nav_in_title(struct dv_ctx *ctx);

/* Pointer in stored-frame coordinates (720 x 480/576). */
void dv_nav_pointer(struct dv_ctx *ctx, int x, int y, bool activate);

#endif
