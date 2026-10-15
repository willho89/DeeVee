/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Presentation clock and stream-PTS -> timeline mapping (CLAUDE.md §5.6).
 * All times are 90 kHz ticks.
 */
#ifndef DV_CLOCK_H
#define DV_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define DV_NOPTS INT64_MIN

typedef struct dv_clock
{
   double  fps;          /* output (retro_run) rate */
   double  tick;         /* 90 kHz ticks per retro_run */
   double  now;          /* presentation clock, timeline units */
   bool    anchored;     /* false: snap 'now' to the next frame presented */

   int64_t offset;       /* timeline = stream_pts + offset */
   int64_t demux_max;    /* furthest A/V PTS demuxed (timeline), DV_NOPTS if unknown */
   int64_t prev_e_ptm;   /* previous VOBU end PTM (stream units) */
   bool    have_prev;
} dv_clock_t;

void    dv_clock_init(dv_clock_t *c, double fps);
void    dv_clock_set_fps(dv_clock_t *c, double fps);
void    dv_clock_advance(dv_clock_t *c);

/* Re-anchor on the next presented frame (after still/wait/hop/seek). */
void    dv_clock_unanchor(dv_clock_t *c);
void    dv_clock_anchor(dv_clock_t *c, int64_t timeline);

/* Called per NAV packet: keeps the timeline continuous across VOBUs whose
 * PTS don't follow on (seamless branching, menu loops, angle changes).
 * Returns true if the offset changed. */
bool    dv_clock_vobu(dv_clock_t *c, uint32_t vobu_s_ptm, uint32_t vobu_e_ptm);

/* After HOP_CHANNEL: re-anchor, keeping VOBU continuity. */
void    dv_clock_hop(dv_clock_t *c);

int64_t dv_clock_to_timeline(const dv_clock_t *c, int64_t stream_pts);

/* Track how far ahead of the clock demuxed audio/video reaches. */
void    dv_clock_demuxed(dv_clock_t *c, int64_t timeline);

/* True when buffered A/V content already reaches 'lead' ticks past the
 * clock: stop reading. */
bool    dv_clock_buffered_ahead(const dv_clock_t *c, double lead);

/* True when the clock has presented everything demuxed (WAIT/still). */
bool    dv_clock_drained(const dv_clock_t *c);

#endif
