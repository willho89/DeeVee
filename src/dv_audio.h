/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Audio decode (AC-3, DTS, LPCM, MPEG-1 L1/L2) -> 48 kHz stereo s16, queued
 * on the presentation timeline (CLAUDE.md §5.6, §5.7).
 */
#ifndef DV_AUDIO_H
#define DV_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dv_ctx;
struct AVCodecContext;
struct AVCodecParserContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

#define DV_AUDIO_RATE        48000
#define DV_AUDIO_CHUNK       4096   /* stereo frames per chunk */
#define DV_AUDIO_CHUNKS      160    /* ~13.6 s of capacity */

typedef struct dv_achunk
{
   int16_t *data;     /* interleaved stereo, DV_AUDIO_CHUNK frames */
   int      frames;   /* valid frames */
   int      off;      /* frames already consumed */
   int64_t  pts;      /* timeline of data[0] */
   bool     seg_start;/* first chunk after a PTS discontinuity: realign exactly */
   bool     seg_end;  /* last chunk before one: fade out its tail */
} dv_achunk_t;

typedef struct dv_audio
{
   int      active_phys;      /* physical stream 0..7 selected by the VM, -1 none */
   unsigned cur_id;           /* PS id the decoder is open for, 0 = none */

   struct AVCodecContext       *avctx;
   struct AVCodecParserContext *parser;   /* NULL for LPCM */
   struct AVPacket             *pkt;
   struct AVFrame              *frame;

   struct SwrContext *swr;
   int swr_rate, swr_fmt, swr_channels;
   uint64_t swr_mask;
   int16_t *conv;             /* swr output scratch */
   int      conv_cap;         /* frames */

   dv_achunk_t chunks[DV_AUDIO_CHUNKS];
   int qhead, qcount;

   int64_t  next_pts;         /* interpolation when a block has no PTS */
   bool     faded_in;         /* last output ended on real audio */
   int64_t  pending_dev;      /* unconfirmed PTS deviation from the queue end, 0 = none */
   int64_t  pending_pts;      /* ...the block that showed it: its PTS */
   int      pending_frames;   /* ...and size (still at the queue tail) */

   /* stats */
   uint32_t packets, decoded_blocks, errors, reopen, overflow, segments, suspect;
   uint64_t out_frames, silence_frames, dropped_frames, gap_frames;
} dv_audio_t;

bool dv_audio_init(struct dv_ctx *ctx);
void dv_audio_free(struct dv_ctx *ctx);

/* Feed one audio PES payload for stream 'id' (dv_ps ids); pts in timeline
 * units or DV_NOPTS. Packets for non-selected streams are ignored. */
void dv_audio_push(struct dv_ctx *ctx, unsigned id, const uint8_t *data, size_t len, int64_t pts);

/* The VM's active audio stream may have changed (AUDIO_STREAM_CHANGE). */
void dv_audio_select(struct dv_ctx *ctx, int physical);

/* Drop decoder state and queued audio (stream switch, seek). */
void dv_audio_flush(struct dv_ctx *ctx);

/* Produce exactly 'frames' stereo frames for the tick starting at the
 * current clock into out (interleaved s16). */
void dv_audio_render(struct dv_ctx *ctx, int16_t *out, size_t frames);

#endif
