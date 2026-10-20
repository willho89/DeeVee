/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MPEG-1/2 video decode (CLAUDE.md §5.4). libavcodec's mpeg2video decoder
 * (handles MPEG-1 too) behind the mpegvideo parser, which splits PES payloads
 * into whole pictures and carries PTS across. Parsed pictures wait in a
 * compressed FIFO and are decoded only when presentation needs them.
 */
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>

#include "dv_ctx.h"
#include "dv_video.h"

/* Pictures more than this far from the clock mean a discontinuity we failed
 * to model (or a jump): re-anchor instead of racing/holding. */
#define DV_RESYNC_THRESHOLD (90000 * 2)
/* Decoded pictures kept ready beyond the one on screen. */
#define DV_DECODE_AHEAD     3

static bool open_parser(dv_video_t *v)
{
   v->parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
   return v->parser != NULL;
}

bool dv_video_init(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);

   memset(v, 0, sizeof(*v));
   v->next_pts = DV_NOPTS;

   if (!codec)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "mpeg2video decoder not built in.\n");
      return false;
   }
   v->avctx = avcodec_alloc_context3(codec);
   if (!v->avctx)
      return false;
   v->avctx->thread_count = 1;   /* lowest latency for menus (§5.4) */

   if (avcodec_open2(v->avctx, codec, NULL) < 0 || !open_parser(v))
   {
      dv_log(ctx, RETRO_LOG_ERROR, "Failed to open MPEG video decoder.\n");
      return false;
   }
   v->tmp  = av_frame_alloc();
   v->filt = av_frame_alloc();
   return v->tmp && v->filt;
}

/* ---- deinterlacer ----------------------------------------------------- */

static void graph_free(dv_video_t *v)
{
   avfilter_graph_free(&v->graph);
   v->src = v->sink = NULL;
}

static bool graph_build(dv_ctx_t *ctx, const AVFrame *f)
{
   dv_video_t *v = &ctx->video;
   const char *name = v->deint == DV_DEINT_BWDIF_2X ? "bwdif" : "yadif";
   /* mode: 0 = one picture per frame, 1 = one per field.
    * deint=interlaced: progressive (film) frames pass through untouched. */
   const char *args_mode = v->deint == DV_DEINT_YADIF ? "mode=send_frame:deint=interlaced"
                                                      : "mode=send_field:deint=interlaced";
   AVFilterContext *deint = NULL;
   char args[256];
   int ret;

   graph_free(v);
   v->graph = avfilter_graph_alloc();
   if (!v->graph)
      return false;
   v->graph->nb_threads = 1;

   snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=1/90000:pixel_aspect=%d/%d",
            f->width, f->height, f->format,
            f->sample_aspect_ratio.num ? f->sample_aspect_ratio.num : 1,
            f->sample_aspect_ratio.den ? f->sample_aspect_ratio.den : 1);
   ret = avfilter_graph_create_filter(&v->src, avfilter_get_by_name("buffer"), "in", args, NULL, v->graph);
   if (ret >= 0)
      ret = avfilter_graph_create_filter(&deint, avfilter_get_by_name(name), "deint", args_mode, NULL, v->graph);
   if (ret >= 0)
      ret = avfilter_graph_create_filter(&v->sink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, v->graph);
   if (ret >= 0)
      ret = avfilter_link(v->src, 0, deint, 0);
   if (ret >= 0)
      ret = avfilter_link(deint, 0, v->sink, 0);
   if (ret >= 0)
      ret = avfilter_graph_config(v->graph, NULL);
   if (ret < 0)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "deinterlacer (%s) setup failed: %d\n", name, ret);
      graph_free(v);
      v->deint = DV_DEINT_OFF;   /* don't retry every frame */
      return false;
   }
   v->graph_w   = f->width;
   v->graph_h   = f->height;
   v->graph_fmt = f->format;
   dv_log(ctx, RETRO_LOG_INFO, "Deinterlacer: %s (%s)\n", name, args_mode);
   return true;
}

void dv_video_set_deint(dv_ctx_t *ctx, dv_deint_t mode)
{
   dv_video_t *v = &ctx->video;
   if (mode == v->deint)
      return;
   v->deint = mode;
   graph_free(v);   /* rebuilt on the next frame */
}

static void clear_queue(dv_video_t *v)
{
   while (v->qcount)
   {
      av_frame_free(&v->queue[v->qhead].frame);
      v->qhead = (v->qhead + 1) % DV_VIDEO_QUEUE_MAX;
      v->qcount--;
   }
   v->qhead = 0;
}

static void clear_fifo(dv_video_t *v)
{
   while (v->fcount)
   {
      av_packet_free(&v->fifo[v->fhead]);
      v->fhead = (v->fhead + 1) % DV_VIDEO_FIFO_MAX;
      v->fcount--;
   }
   v->fhead = 0;
}

void dv_video_free(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   graph_free(v);
   av_frame_free(&v->filt);
   clear_fifo(v);
   clear_queue(v);
   av_frame_free(&v->current.frame);
   av_frame_free(&v->tmp);
   if (v->parser)
      av_parser_close(v->parser);
   v->parser = NULL;
   avcodec_free_context(&v->avctx);
}

/* Frame period in 90 kHz ticks, honouring repeat_pict: RFF on a 29.97
 * stream adds a field (1.5 frames), which is what turns 23.976 film into a
 * 2/3 cadence at 59.94 Hz. */
static int64_t frame_duration(const dv_video_t *v, const AVFrame *f)
{
   AVRational fr = v->avctx->framerate;
   int64_t period = (fr.num > 0 && fr.den > 0)
      ? (int64_t)90000 * fr.den / fr.num
      : 3003;
   return period * (2 + f->repeat_pict) / 2;
}

/* ---- FIFO (compressed) ------------------------------------------------ */

static void fifo_push(dv_ctx_t *ctx, const uint8_t *data, int size, int64_t pts, int64_t dts)
{
   dv_video_t *v = &ctx->video;
   AVPacket *pkt;

   if (v->fcount == DV_VIDEO_FIFO_MAX)
   {
      /* Shouldn't happen: reading stops at dv_video_fifo_full(). */
      av_packet_free(&v->fifo[v->fhead]);
      v->fhead = (v->fhead + 1) % DV_VIDEO_FIFO_MAX;
      v->fcount--;
      v->fifo_overflow++;
   }

   pkt = av_packet_alloc();
   if (!pkt || av_new_packet(pkt, size) < 0)
   {
      av_packet_free(&pkt);
      v->errors++;
      return;
   }
   memcpy(pkt->data, data, (size_t)size);
   pkt->pts = pts;
   pkt->dts = dts;
   v->fifo[(v->fhead + v->fcount) % DV_VIDEO_FIFO_MAX] = pkt;
   v->fcount++;
}

void dv_video_push(dv_ctx_t *ctx, const uint8_t *data, size_t len, int64_t pts)
{
   dv_video_t *v = &ctx->video;
   int64_t in_pts = pts == DV_NOPTS ? AV_NOPTS_VALUE : pts;

   while (len > 0)
   {
      uint8_t *out = NULL;
      int out_size = 0;
      int used = av_parser_parse2(v->parser, v->avctx, &out, &out_size,
                                  data, (int)len, in_pts, AV_NOPTS_VALUE, 0);
      if (used < 0)
      {
         v->errors++;
         return;
      }
      data += used;
      len  -= (size_t)used;
      in_pts = AV_NOPTS_VALUE;  /* PTS belongs to the first picture only */

      if (out_size > 0)
         fifo_push(ctx, out, out_size, v->parser->pts, v->parser->dts);
   }
}

void dv_video_force_still(dv_ctx_t *ctx)
{
   static const uint8_t seq_end[4] = { 0x00, 0x00, 0x01, 0xB7 };
   dv_video_t *v = &ctx->video;
   uint8_t *out = NULL;
   int out_size = 0;

   /* 1. Flush the parser's buffered picture (it only emits a picture once
    *    the next one starts). The parser is recreated afterwards because an
    *    EOF flush leaves it in an end-of-stream state. */
   av_parser_parse2(v->parser, v->avctx, &out, &out_size, NULL, 0,
                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
   if (out_size > 0)
      fifo_push(ctx, out, out_size, v->parser->pts, v->parser->dts);
   av_parser_close(v->parser);
   open_parser(v);

   /* 2. A lone sequence_end_code makes mpeg12dec output its delayed
    *    reference picture without entering EOF (mpeg12dec.c "special case
    *    for last picture"). Same trick as VLC's DemuxForceStill(). Queued in
    *    order, so it takes effect after the pictures before it. */
   fifo_push(ctx, seq_end, sizeof(seq_end), AV_NOPTS_VALUE, AV_NOPTS_VALUE);
}

void dv_video_flush(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   graph_free(v);
   avcodec_flush_buffers(v->avctx);
   av_parser_close(v->parser);
   open_parser(v);
   clear_fifo(v);
   clear_queue(v);
   v->next_pts = DV_NOPTS;
}

/* ---- decode ----------------------------------------------------------- */

/* Final stage: a presentable picture with a valid timeline PTS. */
static void queue_picture(dv_ctx_t *ctx, AVFrame *src, int64_t pts, int64_t duration)
{
   dv_video_t *v = &ctx->video;
   dv_vframe_t *slot;

   if (v->qcount == DV_VIDEO_QUEUE_MAX)
   {
      av_frame_free(&v->queue[v->qhead].frame);
      v->qhead = (v->qhead + 1) % DV_VIDEO_QUEUE_MAX;
      v->qcount--;
      v->overflow++;
   }
   slot = &v->queue[(v->qhead + v->qcount) % DV_VIDEO_QUEUE_MAX];
   slot->frame = av_frame_clone(src);
   if (!slot->frame)
      return;
   slot->pts      = pts;
   slot->duration = duration;
   v->qcount++;
}

/* Pull everything the deinterlacer has ready. */
static void drain_filter(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   AVRational tb, tb90 = { 1, 90000 };

   if (!v->sink)
      return;
   tb = av_buffersink_get_time_base(v->sink);
   while (av_buffersink_get_frame(v->sink, v->filt) >= 0)
   {
      int64_t pts = av_rescale_q(v->filt->pts, tb, tb90);
      queue_picture(ctx, v->filt, pts, 0);
      av_frame_unref(v->filt);
   }
}

/* At a still or end of stream: push out the picture the filter holds back,
 * then start a fresh graph for whatever follows. */
static void flush_filter(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   if (!v->src)
      return;
   if (av_buffersrc_add_frame(v->src, NULL) >= 0)   /* EOF: emit held picture */
      drain_filter(ctx);
   graph_free(v);
}

static void enqueue(dv_ctx_t *ctx, AVFrame *src)
{
   dv_video_t *v = &ctx->video;
   int64_t pts = src->pts, duration;

   v->decoded++;

   if (pts == AV_NOPTS_VALUE)
   {
      if (v->next_pts == DV_NOPTS)
         return;  /* no reference yet: can't place it, skip */
      pts = v->next_pts;
      v->interpolated++;
   }
   duration    = frame_duration(v, src);
   v->next_pts = pts + duration;

   if (v->deint != DV_DEINT_OFF && v->deint != DV_DEINT_CRT)
   {
      if (v->graph && (src->width != v->graph_w || src->height != v->graph_h ||
                       src->format != v->graph_fmt))
         flush_filter(ctx);
      if (v->graph || graph_build(ctx, src))
      {
         src->pts      = pts;
         src->duration = duration;
         if (src->flags & AV_FRAME_FLAG_INTERLACED)
            v->deinterlaced++;
         if (av_buffersrc_add_frame_flags(v->src, src, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0)
         {
            drain_filter(ctx);
            return;
         }
         v->errors++;
      }
   }
   queue_picture(ctx, src, pts, duration);
}

static void receive_all(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   while (avcodec_receive_frame(v->avctx, v->tmp) == 0)
   {
      enqueue(ctx, v->tmp);
      av_frame_unref(v->tmp);
   }
}

static void decode_one(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   AVPacket *pkt = v->fifo[v->fhead];
   int ret;

   v->fifo[v->fhead] = NULL;
   v->fhead = (v->fhead + 1) % DV_VIDEO_FIFO_MAX;
   v->fcount--;

   ret = avcodec_send_packet(v->avctx, pkt);
   if (ret == AVERROR(EAGAIN))
   {
      receive_all(ctx);
      ret = avcodec_send_packet(v->avctx, pkt);
   }
   if (ret < 0 && ret != AVERROR_INVALIDDATA)
      v->errors++;
   receive_all(ctx);
   /* The forced-still marker (a lone sequence_end_code): the decoder has
    * just emitted its held picture; push it through the filter too. */
   if (pkt->size == 4 && pkt->data[3] == 0xB7 && !pkt->data[0] && !pkt->data[1] && pkt->data[2] == 1)
      flush_filter(ctx);
   av_packet_free(&pkt);
}

static int64_t queue_tail_pts(const dv_video_t *v)
{
   return v->queue[(v->qhead + v->qcount - 1) % DV_VIDEO_QUEUE_MAX].pts;
}

void dv_video_decode_ahead(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   const dv_clock_t *c = &ctx->clock;

   while (v->fcount && v->qcount < DV_VIDEO_QUEUE_MAX - 2)
   {
      if (v->qcount)
      {
         if (!c->anchored)
            break;  /* one picture is enough to anchor on */
         if (v->qcount >= DV_DECODE_AHEAD && queue_tail_pts(v) > c->now + c->tick)
            break;
      }
      decode_one(ctx);
   }
}

/* ---- presentation ----------------------------------------------------- */

static void pop_into_current(dv_video_t *v)
{
   dv_vframe_t *head = &v->queue[v->qhead];

   if (v->current.frame)
   {
      unsigned t = v->current_ticks < 8 ? v->current_ticks : 7;
      v->cadence[t]++;
      av_frame_free(&v->current.frame);
   }
   v->current       = *head;
   v->current_new   = true;
   v->current_ticks = 0;
   head->frame      = NULL;
   v->qhead = (v->qhead + 1) % DV_VIDEO_QUEUE_MAX;
   v->qcount--;
   v->presented++;
}

static void maybe_resync(dv_ctx_t *ctx, int64_t head)
{
   dv_clock_t *c = &ctx->clock;
   if (c->anchored &&
       head <= c->now + DV_RESYNC_THRESHOLD &&
       head >= c->now - DV_RESYNC_THRESHOLD)
      return;
   if (c->anchored)
      dv_log(ctx, RETRO_LOG_DEBUG, "video: resync, head %lld vs clock %.0f\n",
             (long long)head, c->now);
   dv_clock_anchor(c, head);
}

void dv_video_present(dv_ctx_t *ctx)
{
   dv_video_t *v = &ctx->video;
   dv_clock_t *c = &ctx->clock;

   dv_video_decode_ahead(ctx);

   while (v->qcount)
   {
      const dv_vframe_t *head = &v->queue[v->qhead];
      const dv_vframe_t *next = v->qcount >= 2
         ? &v->queue[(v->qhead + 1) % DV_VIDEO_QUEUE_MAX] : NULL;
      /* Half a tick of tolerance: a picture due within this tick shows now.
       * Avoids beating between 59.94 Hz ticks and 29.97/25 Hz periods. */
      double now;

      maybe_resync(ctx, head->pts);
      now = c->now + c->tick * 0.5;

      if (head->pts > now)
         break;

      /* A later picture is also due: this one is late, skip it. Only when
       * PTS move forward; a backward step is a discontinuity, handled by
       * resync once that picture reaches the head. */
      if (next && next->pts <= now && next->pts >= head->pts)
      {
         dv_log(ctx, RETRO_LOG_DEBUG, "video: drop late frame pts %lld (clock %.0f, next %lld)\n",
                (long long)head->pts, c->now, (long long)next->pts);
         av_frame_free(&v->queue[v->qhead].frame);
         v->qhead = (v->qhead + 1) % DV_VIDEO_QUEUE_MAX;
         v->qcount--;
         v->dropped++;
         dv_video_decode_ahead(ctx);
         continue;
      }

      pop_into_current(v);
      break;  /* at most one new picture per tick */
   }

   if (v->current.frame)
      v->current_ticks++;
}
