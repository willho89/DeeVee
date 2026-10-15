/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Presentation clock (CLAUDE.md §5.6). libretro calls retro_run at a fixed
 * rate, so the clock advances by 1/fps per call and the newest decoded frame
 * whose timeline PTS <= clock is shown.
 *
 * VOBU continuity follows FFmpeg libavformat/dvdvideodec.c: when a VOBU's
 * start PTM doesn't equal the previous VOBU's end PTM, shift the offset so the
 * new VOBU starts where the previous one ended.
 */
#include "dv_clock.h"

/* Mux lead between demux position and presentation is < ~1 s on DVD;
 * anything much further ahead of a fresh anchor is stale. */
#define DV_CLOCK_MAX_LEAD (90000 * 10)

void dv_clock_init(dv_clock_t *c, double fps)
{
   c->now        = 0.0;
   c->anchored   = false;
   c->offset     = 0;
   c->prev_e_ptm = 0;
   c->have_prev  = false;
   c->demux_max  = DV_NOPTS;
   dv_clock_set_fps(c, fps);
}

void dv_clock_set_fps(dv_clock_t *c, double fps)
{
   c->fps  = fps;
   c->tick = 90000.0 / fps;
}

void dv_clock_advance(dv_clock_t *c)
{
   if (c->anchored)
      c->now += c->tick;
}

void dv_clock_unanchor(dv_clock_t *c)
{
   c->anchored = false;
}

void dv_clock_anchor(dv_clock_t *c, int64_t timeline)
{
   c->now      = (double)timeline;
   c->anchored = true;
   /* Content from before a backward resync would otherwise block reading
    * until the clock caught up with it. */
   if (c->demux_max != DV_NOPTS && c->demux_max > timeline + DV_CLOCK_MAX_LEAD)
      c->demux_max = DV_NOPTS;
}

bool dv_clock_vobu(dv_clock_t *c, uint32_t s_ptm, uint32_t e_ptm)
{
   bool changed = false;

   if (c->have_prev && (int64_t)s_ptm != c->prev_e_ptm)
   {
      c->offset += c->prev_e_ptm - (int64_t)s_ptm;
      changed = true;
   }
   c->prev_e_ptm = e_ptm;
   c->have_prev  = true;
   return changed;
}

void dv_clock_hop(dv_clock_t *c)
{
   /* Keep VOBU continuity (have_prev) so the post-jump content maps after
    * everything already queued and the timeline stays monotonic; just snap
    * the clock to the next picture. */
   c->anchored = false;
}

int64_t dv_clock_to_timeline(const dv_clock_t *c, int64_t pts)
{
   return pts == DV_NOPTS ? DV_NOPTS : pts + c->offset;
}

void dv_clock_demuxed(dv_clock_t *c, int64_t tl)
{
   if (tl != DV_NOPTS && (c->demux_max == DV_NOPTS || tl > c->demux_max))
      c->demux_max = tl;
}

bool dv_clock_buffered_ahead(const dv_clock_t *c, double lead)
{
   return c->anchored && c->demux_max != DV_NOPTS &&
          (double)c->demux_max > c->now + lead;
}

bool dv_clock_drained(const dv_clock_t *c)
{
   return !c->anchored || c->demux_max == DV_NOPTS ||
          (double)c->demux_max <= c->now + c->tick;
}
