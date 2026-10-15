/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MPEG-1/2 video decode -> timestamped frame queue (CLAUDE.md §5.4).
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

#define DV_VIDEO_QUEUE_MAX 16

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
   struct AVPacket             *pkt;
   struct AVFrame              *tmp;

   dv_vframe_t queue[DV_VIDEO_QUEUE_MAX];
   int qhead, qcount;

   dv_vframe_t current;     /* frame on screen (frame NULL until first) */
   bool        current_new; /* changed since last render */
   unsigned    current_ticks;

   int64_t next_pts;        /* interpolation for frames without PTS */

   /* stats */
   uint32_t decoded, presented, dropped, overflow, errors, interpolated;
   uint32_t cadence[8];     /* how many retro_run ticks each frame was shown */
} dv_video_t;

bool dv_video_init(struct dv_ctx *ctx);
void dv_video_free(struct dv_ctx *ctx);

/* Feed one video PES payload; pts is in timeline units or DV_NOPTS. */
void dv_video_push(struct dv_ctx *ctx, const uint8_t *data, size_t len, int64_t pts);

/* Emit the picture held back by the parser/decoder (still frames). Ported
 * from VLC dvdnav.c DemuxForceStill(). */
void dv_video_force_still(struct dv_ctx *ctx);

/* Drop everything (seek). */
void dv_video_flush(struct dv_ctx *ctx);

/* Pick the frame to show at the current clock. Anchors the clock if needed. */
void dv_video_present(struct dv_ctx *ctx);

static inline int dv_video_queued(const dv_video_t *v) { return v->qcount; }
int64_t dv_video_tail_pts(const dv_video_t *v);

#endif
