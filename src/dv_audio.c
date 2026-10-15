/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Audio (CLAUDE.md §5.7). Stream-type handling follows VLC dvdnav.c ESNew()
 * (physical stream = low bits of the substream id) and FFmpeg
 * libavformat/mpeg.c (LPCM packets keep their 3-byte header for pcm_dvd).
 *
 * Decoded audio is kept as timestamped chunks; dv_audio_render() emits the
 * samples for [clock, clock + tick), dropping late audio and padding gaps
 * with silence, so A/V offset is bounded by PTS accuracy and can't drift.
 */
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "dv_ctx.h"
#include "dv_audio.h"

#define TICKS_PER_SAMPLE (90000.0 / DV_AUDIO_RATE)   /* 1.875 */
/* A block within this of the queue's end continues it. Covers PTS rounding
 * and LPCM's PTS granularity (PTS marks the first access unit starting in
 * the packet, up to one 1/600 s frame in); real gaps, e.g. at cell joins,
 * are bigger and keep their PTS position. */
#define DV_AUDIO_JOIN_TOL   (90000 * 5 / 1000)
/* During continuous playback, tolerate this much misalignment before
 * dropping/padding. Playback (re)starts are aligned sample-exactly. */
#define DV_AUDIO_SYNC_TOL   (90000 * 20 / 1000)
/* A PTS jump this big is a real discontinuity without confirmation. */
#define DV_AUDIO_TRUST_JUMP (90000)
#define DV_AUDIO_FADE       64

static unsigned stream_phys(unsigned id)
{
   return id & 0x07;
}

bool dv_audio_init(dv_ctx_t *ctx)
{
   dv_audio_t *a = &ctx->audio;
   int i;

   memset(a, 0, sizeof(*a));
   a->active_phys = -1;
   a->next_pts    = DV_NOPTS;
   a->pkt   = av_packet_alloc();
   a->frame = av_frame_alloc();
   if (!a->pkt || !a->frame)
      return false;
   for (i = 0; i < DV_AUDIO_CHUNKS; i++)
   {
      a->chunks[i].data = malloc(DV_AUDIO_CHUNK * 2 * sizeof(int16_t));
      if (!a->chunks[i].data)
         return false;
   }
   return true;
}

static void close_decoder(dv_audio_t *a)
{
   if (a->parser)
      av_parser_close(a->parser);
   a->parser = NULL;
   avcodec_free_context(&a->avctx);
   a->cur_id = 0;
}

void dv_audio_free(dv_ctx_t *ctx)
{
   dv_audio_t *a = &ctx->audio;
   int i;
   close_decoder(a);
   swr_free(&a->swr);
   free(a->conv);
   a->conv = NULL;
   av_frame_free(&a->frame);
   av_packet_free(&a->pkt);
   for (i = 0; i < DV_AUDIO_CHUNKS; i++)
   {
      free(a->chunks[i].data);
      a->chunks[i].data = NULL;
   }
}

static void clear_queue(dv_audio_t *a)
{
   a->qhead = a->qcount = 0;
   a->next_pts = DV_NOPTS;
   a->pending_dev = 0;
}

void dv_audio_flush(dv_ctx_t *ctx)
{
   dv_audio_t *a = &ctx->audio;
   close_decoder(a);
   clear_queue(a);
}

void dv_audio_select(dv_ctx_t *ctx, int physical)
{
   dv_audio_t *a = &ctx->audio;
   if (physical == a->active_phys)
      return;
   dv_log(ctx, RETRO_LOG_INFO, "Audio: physical stream %d -> %d\n", a->active_phys, physical);
   a->active_phys = physical;
   /* §5.7: on stream switch, flush decoder + buffer. */
   dv_audio_flush(ctx);
}

static bool open_decoder(dv_ctx_t *ctx, unsigned id)
{
   dv_audio_t *a = &ctx->audio;
   enum AVCodecID codec_id;
   const AVCodec *codec;
   AVDictionary *opts = NULL;
   bool use_parser = true;
   const char *name;

   if (DV_PS_IS_AC3(id))       { codec_id = AV_CODEC_ID_AC3;     name = "AC-3"; }
   else if (DV_PS_IS_DTS(id))  { codec_id = AV_CODEC_ID_DTS;     name = "DTS"; }
   else if (DV_PS_IS_LPCM(id)) { codec_id = AV_CODEC_ID_PCM_DVD; name = "LPCM"; use_parser = false; }
   else                        { codec_id = AV_CODEC_ID_MP2;     name = "MPEG audio"; }

   close_decoder(a);

   codec = avcodec_find_decoder(codec_id);
   if (!codec || !(a->avctx = avcodec_alloc_context3(codec)))
   {
      dv_log(ctx, RETRO_LOG_ERROR, "Audio: no %s decoder.\n", name);
      return false;
   }
   a->avctx->thread_count = 1;

   /* Let AC-3/DTS downmix themselves: they honour the stream's own centre /
    * surround mix levels. swresample handles everything else. */
   if (codec_id == AV_CODEC_ID_AC3 || codec_id == AV_CODEC_ID_DTS)
      av_dict_set(&opts, "downmix", "stereo", 0);

   if (avcodec_open2(a->avctx, codec, &opts) < 0)
   {
      av_dict_free(&opts);
      avcodec_free_context(&a->avctx);
      dv_log(ctx, RETRO_LOG_ERROR, "Audio: failed to open %s decoder.\n", name);
      return false;
   }
   av_dict_free(&opts);

   if (use_parser)
      a->parser = av_parser_init(codec_id);

   a->cur_id = id;
   a->reopen++;
   dv_log(ctx, RETRO_LOG_INFO, "Audio: %s, stream 0x%04X (physical %u)\n",
          name, id, stream_phys(id));
   return true;
}

/* ---- queue ------------------------------------------------------------ */

static dv_achunk_t *tail_chunk(dv_audio_t *a)
{
   return a->qcount ? &a->chunks[(a->qhead + a->qcount - 1) % DV_AUDIO_CHUNKS] : NULL;
}

static int64_t chunk_end(const dv_achunk_t *c)
{
   return c->pts + (int64_t)(c->frames * TICKS_PER_SAMPLE + 0.5);
}

static dv_achunk_t *new_chunk(dv_audio_t *a, int64_t pts)
{
   dv_achunk_t *c;
   if (a->qcount == DV_AUDIO_CHUNKS)
   {
      /* Full (reading is gated well before this): drop the oldest. */
      a->dropped_frames += (uint64_t)(a->chunks[a->qhead].frames - a->chunks[a->qhead].off);
      a->qhead = (a->qhead + 1) % DV_AUDIO_CHUNKS;
      a->qcount--;
      a->overflow++;
   }
   c = &a->chunks[(a->qhead + a->qcount) % DV_AUDIO_CHUNKS];
   c->frames    = 0;
   c->off       = 0;
   c->pts       = pts;
   c->seg_start = false;
   c->seg_end   = false;
   a->qcount++;
   return c;
}

/* A discontinuity was just confirmed. The block that first showed it (and
 * any untimestamped blocks after it) was appended contiguously; if still
 * unplayed at the tail, take it back out so it starts the new segment at
 * its own PTS. Returns the (possibly
 * new) tail; *pts becomes the segment start. */
static dv_achunk_t *replace_pending(dv_audio_t *a, dv_achunk_t *t, int64_t *pts)
{
   int n = a->pending_frames;
   int16_t *tmp;

   if (n <= 0 || t->frames - t->off < n || n > DV_AUDIO_CHUNK)
      return t;   /* already (partly) played or spans chunks: leave it */
   tmp = malloc((size_t)n * 2 * sizeof(int16_t));
   if (!tmp)
      return t;

   memcpy(tmp, t->data + (t->frames - n) * 2, (size_t)n * 2 * sizeof(int16_t));
   t->frames -= n;
   t->seg_end = true;
   a->segments++;

   t = new_chunk(a, a->pending_pts);
   t->seg_start = true;
   memcpy(t->data, tmp, (size_t)n * 2 * sizeof(int16_t));
   t->frames = n;
   free(tmp);

   *pts = chunk_end(t);   /* the confirming block follows straight on */
   return t;
}

static void enqueue(dv_ctx_t *ctx, const int16_t *s, int frames, int64_t pts)
{
   dv_audio_t *a = &ctx->audio;
   dv_achunk_t *t = tail_chunk(a);

   if (pts == DV_NOPTS)
   {
      /* Untimestamped (most frames: a PES carries one PTS for several):
       * follows on, and belongs with any pending block for re-placement. */
      if (!t && a->next_pts == DV_NOPTS)
         return;   /* nothing to place it against yet */
      pts = t ? chunk_end(t) : a->next_pts;
      if (a->pending_dev != 0)
         a->pending_frames += frames;
   }
   else if (t)
   {
      int64_t dev = pts - chunk_end(t);
      bool split;

      if (llabs(dev) <= DV_AUDIO_JOIN_TOL)
      {
         split = false;                       /* follows on */
         a->pending_dev = 0;
      }
      else if (llabs(dev) >= DV_AUDIO_TRUST_JUMP)
         split = true;                        /* unambiguous jump */
      else if (a->pending_dev != 0 && llabs(dev - a->pending_dev) <= DV_AUDIO_JOIN_TOL)
      {
         split = true;                        /* confirmed by a second PTS */
         t = replace_pending(a, t, &pts);
      }
      else
      {
         /* Unconfirmed: libavcodec's parsers occasionally give two
          * consecutive frames the same PTS when a frame straddles PES
          * packets (seen with DTS: +1 frame, then -1 frame). Keep
          * continuity and act only if the next PTS agrees. */
         a->pending_dev    = dev;
         a->pending_pts    = pts;
         a->pending_frames = frames;
         a->suspect++;
         split = false;
      }

      if (!split || pts == chunk_end(t))
      {
         if (split)
            a->pending_dev = 0;   /* handled by replace_pending() */
         pts = chunk_end(t);
      }
      else
      {
         a->pending_dev = 0;
         if (pts > chunk_end(t))
            a->gap_frames += (uint64_t)((pts - chunk_end(t)) / TICKS_PER_SAMPLE);
         a->segments++;
         t->seg_end = true;
         t = new_chunk(a, pts);
         t->seg_start = true;
      }
   }

   while (frames > 0)
   {
      int n;
      if (!t || t->frames == DV_AUDIO_CHUNK)
         t = new_chunk(a, t ? chunk_end(t) : pts);
      n = DV_AUDIO_CHUNK - t->frames;
      if (n > frames)
         n = frames;
      memcpy(t->data + t->frames * 2, s, (size_t)n * 2 * sizeof(int16_t));
      t->frames += n;
      s         += n * 2;
      frames    -= n;
   }
   a->next_pts = chunk_end(t);
}

/* ---- decode ----------------------------------------------------------- */

static bool setup_swr(dv_ctx_t *ctx, const AVFrame *f)
{
   dv_audio_t *a = &ctx->audio;
   AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
   uint64_t mask = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;

   if (a->swr && a->swr_rate == f->sample_rate && a->swr_fmt == f->format &&
       a->swr_channels == f->ch_layout.nb_channels && a->swr_mask == mask)
      return true;

   swr_free(&a->swr);
   if (swr_alloc_set_opts2(&a->swr, &stereo, AV_SAMPLE_FMT_S16, DV_AUDIO_RATE,
                           &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0)
      return false;
   /* Normalise the downmix matrix so nothing can clip. */
   av_opt_set_double(a->swr, "rematrix_maxval", 1.0, 0);
   if (swr_init(a->swr) < 0)
   {
      swr_free(&a->swr);
      return false;
   }
   a->swr_rate     = f->sample_rate;
   a->swr_fmt      = f->format;
   a->swr_channels = f->ch_layout.nb_channels;
   a->swr_mask     = mask;
   dv_log(ctx, RETRO_LOG_INFO, "Audio: %d Hz, %d ch, %s -> 48000 Hz stereo s16\n",
          f->sample_rate, f->ch_layout.nb_channels,
          av_get_sample_fmt_name((enum AVSampleFormat)f->format));
   return true;
}

static void convert_frame(dv_ctx_t *ctx, const AVFrame *f)
{
   dv_audio_t *a = &ctx->audio;
   int cap, out;
   int64_t pts = f->pts == AV_NOPTS_VALUE ? DV_NOPTS : f->pts;

   if (!setup_swr(ctx, f))
   {
      a->errors++;
      return;
   }

   cap = swr_get_out_samples(a->swr, f->nb_samples);
   if (cap <= 0)
      return;
   if (cap > a->conv_cap)
   {
      int16_t *n = realloc(a->conv, (size_t)cap * 2 * sizeof(int16_t));
      if (!n)
         return;
      a->conv     = n;
      a->conv_cap = cap;
   }
   out = swr_convert(a->swr, (uint8_t **)&a->conv, cap,
                     (const uint8_t **)f->extended_data, f->nb_samples);
   if (out <= 0)
      return;

   /* swr's resampling delay (96k LPCM) shifts output by a few samples;
    * well inside the join tolerance, so continuity wins. */
   a->decoded_blocks++;
   enqueue(ctx, a->conv, out, pts);
}

static void decode_packet(dv_ctx_t *ctx, const uint8_t *data, int size, int64_t pts)
{
   dv_audio_t *a = &ctx->audio;
   int ret;

   a->pkt->data = (uint8_t *)data;
   a->pkt->size = size;
   a->pkt->pts  = pts == DV_NOPTS ? AV_NOPTS_VALUE : pts;
   a->pkt->dts  = AV_NOPTS_VALUE;

   ret = avcodec_send_packet(a->avctx, a->pkt);
   if (ret < 0 && ret != AVERROR(EAGAIN))
   {
      a->errors++;
      return;
   }
   while (avcodec_receive_frame(a->avctx, a->frame) == 0)
   {
      convert_frame(ctx, a->frame);
      av_frame_unref(a->frame);
   }
}

void dv_audio_push(dv_ctx_t *ctx, unsigned id, const uint8_t *data, size_t len, int64_t pts)
{
   dv_audio_t *a = &ctx->audio;

   if (a->active_phys < 0 || stream_phys(id) != (unsigned)a->active_phys)
      return;
   if (id != a->cur_id && !open_decoder(ctx, id))
      return;

   a->packets++;
   dv_clock_demuxed_audio(&ctx->clock, pts);

   if (!a->parser)
   {
      decode_packet(ctx, data, (int)len, pts);
      return;
   }

   while (len > 0)
   {
      uint8_t *out = NULL;
      int out_size = 0;
      int used = av_parser_parse2(a->parser, a->avctx, &out, &out_size, data, (int)len,
                                  pts == DV_NOPTS ? AV_NOPTS_VALUE : pts,
                                  AV_NOPTS_VALUE, 0);
      if (used < 0)
      {
         a->errors++;
         return;
      }
      data += used;
      len  -= (size_t)used;
      pts   = DV_NOPTS;   /* belongs to the first frame starting here */
      if (out_size > 0)
         decode_packet(ctx, out, out_size,
                       a->parser->pts == AV_NOPTS_VALUE ? DV_NOPTS : a->parser->pts);
   }
}

/* ---- output ----------------------------------------------------------- */

static void fade_out(int16_t *s, size_t frames)
{
   size_t i, n = frames < DV_AUDIO_FADE ? frames : DV_AUDIO_FADE;
   int16_t *p = s + (frames - n) * 2;
   for (i = 0; i < n; i++)
   {
      int k = (int)(n - 1 - i);
      p[2 * i]     = (int16_t)(p[2 * i]     * k / DV_AUDIO_FADE);
      p[2 * i + 1] = (int16_t)(p[2 * i + 1] * k / DV_AUDIO_FADE);
   }
}

static void fade_in(int16_t *s, size_t frames)
{
   size_t i, n = frames < DV_AUDIO_FADE ? frames : DV_AUDIO_FADE;
   for (i = 0; i < n; i++)
   {
      s[2 * i]     = (int16_t)(s[2 * i]     * (int)i / DV_AUDIO_FADE);
      s[2 * i + 1] = (int16_t)(s[2 * i + 1] * (int)i / DV_AUDIO_FADE);
   }
}

void dv_audio_render(dv_ctx_t *ctx, int16_t *out, size_t frames)
{
   dv_audio_t *a = &ctx->audio;
   const dv_clock_t *c = &ctx->clock;
   double target = c->now;   /* timeline of out[0] */
   size_t done = 0;

   if (!c->anchored)
   {
      memset(out, 0, frames * 2 * sizeof(int16_t));
      a->silence_frames += frames;
      a->faded_in = false;
      return;
   }

   while (done < frames)
   {
      dv_achunk_t *h;
      double t, tol;
      size_t n;

      if (!a->qcount)
         break;

      h = &a->chunks[a->qhead];
      if (h->off >= h->frames)
      {
         a->qhead = (a->qhead + 1) % DV_AUDIO_CHUNKS;
         a->qcount--;
         continue;
      }

      t = (double)h->pts + h->off * TICKS_PER_SAMPLE;
      /* Starting from silence: align to the sample. A tolerance here would
       * become a permanent offset for the whole contiguous run (e.g. the
       * encoder priming that starts AC-3/MP2 a few ms before the video). */
      tol = (a->faded_in && !(h->seg_start && h->off == 0))
            ? DV_AUDIO_SYNC_TOL : TICKS_PER_SAMPLE;

      if (t < target - tol)
      {

         /* Late: skip to the target. */
         size_t skip = (size_t)((target - t) / TICKS_PER_SAMPLE);
         size_t left = (size_t)(h->frames - h->off);
         if (skip > left)
            skip = left;
         if (skip == 0)
            skip = 1;
         h->off += (int)skip;
         a->dropped_frames += skip;
         a->faded_in = false;
         continue;
      }

      if (t > target + tol)
      {
         /* Early: silence until it's due. */
         size_t pad = (size_t)((t - target) / TICKS_PER_SAMPLE);
         if (pad > frames - done)
            pad = frames - done;
         if (pad == 0)
            pad = 1;
         memset(out + done * 2, 0, pad * 2 * sizeof(int16_t));
         done   += pad;
         target += pad * TICKS_PER_SAMPLE;
         a->silence_frames += pad;
         a->faded_in = false;
         continue;
      }

      n = (size_t)(h->frames - h->off);
      if (n > frames - done)
         n = frames - done;
      memcpy(out + done * 2, h->data + h->off * 2, n * 2 * sizeof(int16_t));
      if (!a->faded_in || (h->seg_start && h->off == 0))
      {
         fade_in(out + done * 2, n);
         a->faded_in = true;
      }
      h->off += (int)n;
      if (h->seg_end && h->off == h->frames)
         fade_out(out + done * 2, n);
      done   += n;
      target += n * TICKS_PER_SAMPLE;
      a->out_frames += n;
   }

   if (done < frames)
   {
      memset(out + done * 2, 0, (frames - done) * 2 * sizeof(int16_t));
      a->silence_frames += frames - done;
      a->faded_in = false;
   }
}
