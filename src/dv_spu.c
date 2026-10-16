/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * DVD subpicture decoder (CLAUDE.md §5.5).
 *
 * Reassembly and control-sequence parsing ported from VLC
 * modules/codec/spudec/spudec.c Reassemble() and parse.c ParseControlSeq()
 * @ 843607a066be (3.0.x); RLE decoding follows FFmpeg
 * libavcodec/dvdsubdec.c decode_run_2bit()/decode_rle() (n8.1.3).
 *
 * Original code:
 *   VLC: Copyright (C) 2000-2001, 2005, 2006 VLC authors and VideoLAN
 *        Authors: Sam Hocevar <sam@zoy.org>, LGPL-2.1-or-later
 *   FFmpeg: Copyright (c) 2005 Fabrice Bellard, LGPL-2.1-or-later
 * Adapted for DeeVee (combined work distributed under GPL-2.0-or-later).
 */
#include <stdlib.h>
#include <string.h>

#include "dv_ctx.h"
#include "dv_spu.h"

/* DCSQ dates are in units of 1024 / 90000 s. */
#define DV_SPU_DATE_TICKS 1024

#define SPU_CMD_FORCE_DISPLAY  0x00
#define SPU_CMD_START_DISPLAY  0x01
#define SPU_CMD_STOP_DISPLAY   0x02
#define SPU_CMD_SET_PALETTE    0x03
#define SPU_CMD_SET_ALPHA      0x04
#define SPU_CMD_SET_COORDS     0x05
#define SPU_CMD_SET_OFFSETS    0x06
#define SPU_CMD_SET_COLCON     0x07
#define SPU_CMD_END            0xFF

static unsigned be16(const uint8_t *p)
{
   return ((unsigned)p[0] << 8) | p[1];
}

bool dv_spu_init(dv_ctx_t *ctx)
{
   dv_spu_t *s = &ctx->spu;
   memset(s, 0, sizeof(*s));
   s->stream = -1;
   s->shown  = -1;
   return true;
}

static void free_subpic(dv_subpic_t *p)
{
   free(p->pix);
   p->pix = NULL;
}

static void drop_front(dv_spu_t *s, int n)
{
   int i;
   for (i = 0; i < n; i++)
      free_subpic(&s->q[i]);
   memmove(s->q, s->q + n, (size_t)(s->qcount - n) * sizeof(s->q[0]));
   s->qcount -= n;
   if (s->shown >= 0)
      s->shown = s->shown >= n ? s->shown - n : -1;
}

void dv_spu_clear(dv_ctx_t *ctx)
{
   dv_spu_t *s = &ctx->spu;
   if (s->shown >= 0)
      s->changed = true;
   drop_front(s, s->qcount);
   s->shown = -1;
   s->have = s->need = 0;
}

void dv_spu_free(dv_ctx_t *ctx)
{
   dv_spu_clear(ctx);
   free(ctx->spu.buf);
   ctx->spu.buf = NULL;
}

void dv_spu_select(dv_ctx_t *ctx, int active)
{
   dv_spu_t *s = &ctx->spu;
   /* dvdnav_get_active_spu_stream() returns int8_t: -1 is "none", but a
    * hidden stream (bit 7 set) also comes back negative, e.g. 0x80 = -128. */
   unsigned u  = (unsigned)active & 0xFF;
   int  stream = active == -1 ? -1 : (int)(u & 0x1F);
   bool hidden = active != -1 && (u & 0x80);

   if (hidden != s->hidden)
   {
      s->hidden  = hidden;
      s->changed = true;
   }
   if (stream == s->stream)
      return;

   dv_log(ctx, RETRO_LOG_INFO, "Subpicture: stream %d -> %d%s\n", s->stream, stream,
          hidden ? " (hidden: forced only)" : "");
   s->stream = stream;
   dv_spu_clear(ctx);
}

/* ---- RLE (FFmpeg dvdsubdec.c decode_run_2bit / decode_rle) ------------- */

typedef struct
{
   const uint8_t *buf;
   size_t nibble, end_nibble;
} nibbles_t;

static int get_nibble(nibbles_t *n)
{
   unsigned v;
   if (n->nibble >= n->end_nibble)
      return -1;
   v = n->buf[n->nibble >> 1];
   v = (n->nibble & 1) ? (v & 0x0F) : (v >> 4);
   n->nibble++;
   return (int)v;
}

/* Variable-length code: 1..4 nibbles, value = run << 2 | colour; a run of 0
 * fills the rest of the line. Returns the run (INT32_MAX = to end of line). */
static int decode_run(nibbles_t *n, int *color)
{
   unsigned v = 0, t;
   for (t = 1; v < t && t <= 0x40; t <<= 2)
   {
      int nib = get_nibble(n);
      if (nib < 0)
         return -1;
      v = (v << 4) | (unsigned)nib;
   }
   *color = (int)(v & 3);
   return v < 4 ? INT32_MAX : (int)(v >> 2);
}

/* Decode one field (every second line starting at 'first'). */
static bool decode_field(const uint8_t *buf, size_t start, size_t end,
                         uint8_t *pix, int w, int h, int first)
{
   nibbles_t n;
   int y;

   if (start >= end)
      return false;
   n.buf        = buf;
   n.nibble     = start * 2;
   n.end_nibble = end * 2;

   for (y = first; y < h; y += 2)
   {
      uint8_t *row = pix + (size_t)y * (size_t)w;
      int x = 0;
      while (x < w)
      {
         int color, len = decode_run(&n, &color);
         if (len < 0)
            return y > first;   /* ran out: keep what we have if any */
         if (len > w - x)
            len = w - x;        /* clamp overlong runs (VLC-tolerant) */
         memset(row + x, color, (size_t)len);
         x += len;
      }
      n.nibble = (n.nibble + 1) & ~(size_t)1;   /* byte align per line */
   }
   return true;
}

/* ---- control sequences (VLC parse.c ParseControlSeq) ------------------- */

static void parse_packet(dv_ctx_t *ctx, const uint8_t *buf, size_t size, int64_t pts)
{
   dv_spu_t *s = &ctx->spu;
   dv_subpic_t p;
   size_t ctrl = be16(buf + 2), i, cur, next;
   long start_date = -1, stop_date = -1;
   long off[2] = { -1, -1 };
   bool have_area = false;
   int guard = 0;

   memset(&p, 0, sizeof(p));
   /* VLC defaults: background transparent, the rest opaque. */
   p.alpha[0] = 0x0; p.alpha[1] = 0xF; p.alpha[2] = 0xF; p.alpha[3] = 0xF;
   p.color[0] = 0;   p.color[1] = 1;   p.color[2] = 2;   p.color[3] = 3;

   if (ctrl < 4 || ctrl >= size)
      goto fail;

   for (i = ctrl; i + 4 <= size && guard++ < 64; )
   {
      unsigned date = be16(buf + i);
      bool end = false;

      cur  = i;
      next = be16(buf + i + 2);
      i += 4;

      while (!end && i < size)
      {
         switch (buf[i])
         {
            case SPU_CMD_FORCE_DISPLAY:
               p.forced = true;
               /* fall through */
            case SPU_CMD_START_DISPLAY:
               if (start_date < 0)
                  start_date = (long)date;
               i += 1;
               break;
            case SPU_CMD_STOP_DISPLAY:
               stop_date = (long)date;
               i += 1;
               break;
            case SPU_CMD_SET_PALETTE:
               if (i + 3 > size) goto fail;
               p.color[3] = buf[i + 1] >> 4;  p.color[2] = buf[i + 1] & 0x0F;
               p.color[1] = buf[i + 2] >> 4;  p.color[0] = buf[i + 2] & 0x0F;
               i += 3;
               break;
            case SPU_CMD_SET_ALPHA:
               if (i + 3 > size) goto fail;
               p.alpha[3] = buf[i + 1] >> 4;  p.alpha[2] = buf[i + 1] & 0x0F;
               p.alpha[1] = buf[i + 2] >> 4;  p.alpha[0] = buf[i + 2] & 0x0F;
               i += 3;
               break;
            case SPU_CMD_SET_COORDS:
            {
               int x0, x1, y0, y1;
               if (i + 7 > size) goto fail;
               x0 = (buf[i + 1] << 4) | (buf[i + 2] >> 4);
               x1 = ((buf[i + 2] & 0x0F) << 8) | buf[i + 3];
               y0 = (buf[i + 4] << 4) | (buf[i + 5] >> 4);
               y1 = ((buf[i + 5] & 0x0F) << 8) | buf[i + 6];
               if (x1 < x0 || y1 < y0) goto fail;
               p.x = x0; p.w = x1 - x0 + 1;
               p.y = y0; p.h = y1 - y0 + 1;
               have_area = true;
               i += 7;
               break;
            }
            case SPU_CMD_SET_OFFSETS:
               if (i + 5 > size) goto fail;
               off[0] = (long)be16(buf + i + 1);
               off[1] = (long)be16(buf + i + 3);
               i += 5;
               break;
            case SPU_CMD_SET_COLCON:
               /* Per-region colour/contrast changes: rare; M8. Skip it. */
               if (i + 3 > size) goto fail;
               s->unsupported++;
               i += 1 + be16(buf + i + 1);
               break;
            case SPU_CMD_END:
               end = true;
               i += 1;
               break;
            default:
               /* Unknown command: skip to the next sequence like VLC. */
               s->unsupported++;
               end = true;
               break;
         }
      }

      if (next <= cur || next >= size)
         break;   /* last DCSQ points at itself */
      i = next;
   }

   if (start_date < 0 || !have_area || off[0] < 4 || off[1] < 4 ||
       (size_t)off[0] >= ctrl || (size_t)off[1] >= ctrl ||
       p.w <= 0 || p.h <= 0 || p.w > 720 || p.h > 576)
      goto fail;

   p.pix = calloc((size_t)p.w * (size_t)p.h, 1);
   if (!p.pix)
      goto fail;
   /* Top field (even lines) and bottom field (odd lines); RLE ends where the
    * control area begins. */
   if (!decode_field(buf, (size_t)off[0], ctrl, p.pix, p.w, p.h, 0) ||
       !decode_field(buf, (size_t)off[1], ctrl, p.pix, p.w, p.h, 1))
   {
      free(p.pix);
      goto fail;
   }

   p.start = pts + (int64_t)start_date * DV_SPU_DATE_TICKS;
   p.stop  = stop_date >= 0 ? pts + (int64_t)stop_date * DV_SPU_DATE_TICKS : DV_NOPTS;
   if (p.stop != DV_NOPTS && p.stop <= p.start)
      p.stop = DV_NOPTS;

   if (s->qcount == DV_SPU_QUEUE)
      drop_front(s, 1);
   s->q[s->qcount++] = p;
   s->decoded++;
   dv_log(ctx, RETRO_LOG_DEBUG, "spu: %dx%d at %d,%d start %lld stop %lld%s colors %X%X%X%X alpha %X%X%X%X\n",
          p.w, p.h, p.x, p.y, (long long)p.start, (long long)p.stop,
          p.forced ? " forced" : "", p.color[3], p.color[2], p.color[1], p.color[0],
          p.alpha[3], p.alpha[2], p.alpha[1], p.alpha[0]);
   return;

fail:
   s->errors++;
}

/* ---- reassembly (VLC spudec.c Reassemble) ----------------------------- */

void dv_spu_push(dv_ctx_t *ctx, unsigned id, const uint8_t *data, size_t len, int64_t pts)
{
   dv_spu_t *s = &ctx->spu;

   if (s->stream < 0 || (int)(id & 0x1F) != s->stream)
      return;
   s->packets++;

   /* A packet with a PTS starts a new SPU; otherwise it continues one. */
   if (pts != DV_NOPTS || s->need == 0)
   {
      if (pts == DV_NOPTS || len < 4)
         return;   /* continuation without a start: drop */
      s->need = be16(data);
      s->have = 0;
      s->pts  = pts;
      if (s->need < 4)
      {
         s->need = 0;
         s->errors++;
         return;
      }
      if (s->need > s->cap)
      {
         uint8_t *b = realloc(s->buf, s->need);
         if (!b)
         {
            s->need = 0;
            return;
         }
         s->buf = b;
         s->cap = s->need;
      }
   }

   if (len > s->need - s->have)
      len = s->need - s->have;
   memcpy(s->buf + s->have, data, len);
   s->have += len;

   if (s->have == s->need)
   {
      parse_packet(ctx, s->buf, s->need, s->pts);
      s->need = s->have = 0;
   }
}

/* ---- selection -------------------------------------------------------- */

const dv_subpic_t *dv_spu_update(dv_ctx_t *ctx)
{
   dv_spu_t *s = &ctx->spu;
   const dv_clock_t *c = &ctx->clock;
   double now = c->now + c->tick * 0.5;
   int i, cur = -1, prev = s->shown;
   const dv_subpic_t *p;

   /* Newest subpicture that has started supersedes everything before it.
    * With the clock held (after a WAIT, through a still) no picture will
    * re-anchor it, but VLC's clock keeps running there: show what has
    * started by the held time, and in a still anything demuxed for it
    * (reading has stopped, so the queue belongs to the still). Found on
    * TIMERIC's sub-menus: WAIT -> SPU -> infinite still (M6c). */
   if (!c->anchored)
   {
      bool held_still = ctx->nav.still;
      for (i = 0; i < s->qcount; i++)
         if (held_still || (double)s->q[i].start <= c->now)
            cur = i;
      if (cur < 0)
         return s->shown >= 0 ? &s->q[s->shown] : NULL;
      if (cur > 0)
         drop_front(s, cur);
      p = &s->q[0];
      if (s->hidden && !p->forced)
      {
         if (s->shown >= 0)
            s->changed = true;
         s->shown = -1;
         return NULL;
      }
      if (s->shown != 0)
      {
         s->changed = true;
         s->displayed++;
         dv_log(ctx, RETRO_LOG_DEBUG, "spu: show start %lld (clock held at %.0f)\n",
                (long long)p->start, c->now);
      }
      s->shown = 0;
      return p;
   }

   for (i = 0; i < s->qcount; i++)
      if ((double)s->q[i].start <= now)
         cur = i;
   if (cur > 0)
   {
      drop_front(s, cur);
      prev = s->shown;
      cur = 0;
   }

   p = cur >= 0 ? &s->q[cur] : NULL;
   if (p && p->stop != DV_NOPTS && (double)p->stop <= now)
   {
      drop_front(s, 1);
      p = NULL;
      cur = -1;
   }
   if (p && s->hidden && !p->forced)
      p = NULL, cur = -1;   /* subtitles off: forced only */

   if (cur != prev)
   {
      s->changed = true;
      if (cur >= 0)
      {
         s->displayed++;
         dv_log(ctx, RETRO_LOG_DEBUG, "spu: show start %lld at clock %.0f\n",
                (long long)p->start, c->now);
      }
   }
   s->shown = cur;
   return p;
}
