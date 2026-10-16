/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MPEG-1/2 video (CLAUDE.md §5.4): PES -> parser -> compressed picture FIFO
 * -> decode on demand -> timestamped frame queue -> presentation.
 *
 * The compressed FIFO exists because DVD muxes video ~0.5-1 s ahead of the
 * audio with the same PTS: to have audio buffered for the clock we must read
 * that far ahead, and holding a second of *decoded* SD frames would cost
 * ~15 MB. Compressed it's ~1 MB.
 */
#ifndef DV_VIDEO_H
#define DV_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dv_ctx;
struct AVCodecContext;
struct AVCodecParserContext;
struct AVPacket;
struct AVFrame;
struct AVFilterGraph;
struct AVFilterContext;

typedef enum
{
   DV_DEINT_OFF = 0,
   DV_DEINT_YADIF,       /* frame rate (one picture per frame) */
   DV_DEINT_YADIF_2X,    /* field rate */
   DV_DEINT_BWDIF_2X     /* field rate, sharper */
} dv_deint_t;

#define DV_VIDEO_QUEUE_MAX 16    /* decoded frames */
#define DV_VIDEO_FIFO_MAX  512   /* compressed pictures (~17 s at 29.97) */

typedef struct dv_vframe
{
   struct AVFrame *frame;
   int64_t pts;          /* timeline units, always valid once queued */
   int64_t duration;     /* 90 kHz, includes RFF/repeat_pict */
} dv_vframe_t;

typedef struct dv_video
{
   struct AVCodecContext       *avctx;
   struct AVCodecParserContext *parser;
   struct AVFrame              *tmp;

   /* compressed pictures awaiting decode (decode order) */
   struct AVPacket *fifo[DV_VIDEO_FIFO_MAX];
   int fhead, fcount;

   /* decoded pictures awaiting presentation (display order) */
   dv_vframe_t queue[DV_VIDEO_QUEUE_MAX];
   int qhead, qcount;

   dv_vframe_t current;     /* frame on screen (frame NULL until first) */
   bool        current_new; /* changed since last render */
   unsigned    current_ticks;

   int64_t next_pts;        /* interpolation for frames without PTS */

   /* deinterlacer (libavfilter), rebuilt on format change/flush */
   dv_deint_t deint;
   struct AVFilterGraph   *graph;
   struct AVFilterContext *src, *sink;
   int graph_w, graph_h, graph_fmt;
   struct AVFrame *filt;

   /* stats */
   uint32_t decoded, presented, dropped, overflow, errors, interpolated, fifo_overflow, deinterlaced;
   uint32_t cadence[8];     /* how many retro_run ticks each frame was shown */
} dv_video_t;

bool dv_video_init(struct dv_ctx *ctx);
void dv_video_free(struct dv_ctx *ctx);

/* Feed one video PES payload; pts is in timeline units or DV_NOPTS. */
void dv_video_push(struct dv_ctx *ctx, const uint8_t *data, size_t len, int64_t pts);

/* Emit the picture held back by the parser/decoder (still frames). Ported
 * from VLC dvdnav.c DemuxForceStill(). */
void dv_video_force_still(struct dv_ctx *ctx);

/* Change the deinterlacer (core option). */
void dv_video_set_deint(struct dv_ctx *ctx, dv_deint_t mode);

/* Drop everything (seek). */
void dv_video_flush(struct dv_ctx *ctx);

/* Decode from the FIFO until a picture beyond the clock is queued (or, when
 * the clock isn't anchored, until there's one picture to anchor on). */
void dv_video_decode_ahead(struct dv_ctx *ctx);

/* Pick the frame to show at the current clock. Anchors the clock if needed. */
void dv_video_present(struct dv_ctx *ctx);

/* Nothing left to show: FIFO and decoded queue both empty. */
static inline bool dv_video_empty(const dv_video_t *v) { return v->qcount == 0 && v->fcount == 0; }
static inline bool dv_video_fifo_full(const dv_video_t *v) { return v->fcount >= DV_VIDEO_FIFO_MAX - 8; }

#endif
