/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * DVD subpicture (SPU) decoder (CLAUDE.md §5.5): reassembly, control
 * sequence (DCSQ) parsing, RLE -> 2-bit index bitmap, timed selection.
 * The bitmap stays as indices 0..3 so menu highlights (M4) only swap the
 * palette/alpha, never re-decode.
 */
#ifndef DV_SPU_H
#define DV_SPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dv_ctx;

#define DV_SPU_QUEUE 8

typedef struct dv_subpic
{
   int64_t  start, stop;   /* timeline; stop == DV_NOPTS: until replaced */
   bool     forced;        /* FSTA_DSP: shown even when subtitles are off */
   int      x, y, w, h;    /* display area, 720-wide stored-frame space */
   uint8_t  color[4];      /* CLUT index per pixel value (SET_COLOR) */
   uint8_t  alpha[4];      /* 0..15 per pixel value (SET_CONTR) */
   uint8_t *pix;           /* w*h values 0..3 */
} dv_subpic_t;

typedef struct dv_spu
{
   int  stream;            /* physical stream 0..31 being decoded, -1 none */
   bool hidden;            /* VM display flag off: only forced subpictures show */

   /* reassembly of one SPU packet across PES packets */
   uint8_t *buf;
   size_t   cap, have, need;
   int64_t  pts;

   dv_subpic_t q[DV_SPU_QUEUE];  /* by start time */
   int  qcount;
   int  shown;             /* index into q of what's on screen, -1 none */
   bool changed;           /* overlay changed since last composite */

   /* stats */
   uint32_t packets, decoded, errors, unsupported, displayed;
} dv_spu_t;

bool dv_spu_init(struct dv_ctx *ctx);
void dv_spu_free(struct dv_ctx *ctx);

/* Selection from dvdnav_get_active_spu_stream() (int8_t): -1 none, else
 * physical stream with bit 7 set when subtitles are off (forced only) --
 * which makes those values negative too. */
void dv_spu_select(struct dv_ctx *ctx, int active);

/* Feed one SPU PES payload (after the substream id); pts in timeline units. */
void dv_spu_push(struct dv_ctx *ctx, unsigned id, const uint8_t *data, size_t len, int64_t pts);

/* Drop queued/visible subpictures (jumps, domain changes). */
void dv_spu_clear(struct dv_ctx *ctx);

/* Pick what's on screen for the current clock. Returns the visible
 * subpicture or NULL; ctx->spu.changed tells whether that differs from the
 * last call. */
const dv_subpic_t *dv_spu_update(struct dv_ctx *ctx);

#endif
