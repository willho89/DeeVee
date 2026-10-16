/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * yuv420p -> XRGB8888 via swscale, BT.601 limited range (CLAUDE.md §5.4).
 */
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "dv_ctx.h"
#include "dv_render.h"

/* libretro XRGB8888 is a native-endian 0x00RRGGBB uint32. */
#define DV_RGB_FMT AV_PIX_FMT_0RGB32

/* Letterbox: picture in the middle 3/4 of the lines. Pan&scan: a 3/4-width
 * window, stretched to the full width. */
static unsigned lb_top(unsigned h)  { return h / 8; }
static unsigned ps_width(unsigned w) { return (w * 3 / 4) & ~1u; }

bool dv_render_init(dv_ctx_t *ctx)
{
   dv_render_t *r = &ctx->render;
   memset(r, 0, sizeof(*r));
   r->buf  = calloc((size_t)DV_MAX_WIDTH * DV_MAX_HEIGHT, sizeof(uint32_t));
   r->xbuf = calloc((size_t)DV_MAX_WIDTH * DV_MAX_HEIGHT, sizeof(uint32_t));
   r->width  = 720;
   r->height = 480;
   r->dar    = 4.0 / 3.0;
   r->xsws_mode = -1;
   return r->buf && r->xbuf;
}

void dv_render_free(dv_ctx_t *ctx)
{
   dv_render_t *r = &ctx->render;
   sws_freeContext(r->sws);
   r->sws = NULL;
   sws_freeContext(r->xsws);
   r->xsws = NULL;
   free(r->buf);
   r->buf = NULL;
   free(r->xbuf);
   r->xbuf = NULL;
}

/* DAR from the stream (sequence header aspect -> SAR), never from pixel
 * dimensions alone (§5.4). Falls back to the IFO aspect for the VTS. */
static double frame_dar(dv_ctx_t *ctx, const AVFrame *f)
{
   if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
      return (double)f->sample_aspect_ratio.num * f->width /
             ((double)f->sample_aspect_ratio.den * f->height);
   return ctx->nav.ifo_aspect == 3 ? 16.0 / 9.0 : 4.0 / 3.0;
}

/* Left edge of the pan&scan window. MPEG-2 frame_centre_horizontal_offset
 * (1/16 px) is positive when the frame centre lies right of the display
 * window's centre (ISO 13818-2 §6.3.12); most discs leave it 0 (centre). */
static int panscan_left(const AVFrame *f)
{
   const AVFrameSideData *sd = av_frame_get_side_data(f, AV_FRAME_DATA_PANSCAN);
   int cw = (int)ps_width((unsigned)f->width);
   int x0 = (f->width - cw) / 2;

   if (sd && sd->size >= sizeof(AVPanScan))
      x0 -= ((const AVPanScan *)sd->data)->position[0][0] / 16;
   if (x0 < 0)
      x0 = 0;
   if (x0 > f->width - cw)
      x0 = f->width - cw;
   return x0 & ~1;
}

bool dv_render_frame(dv_ctx_t *ctx, const AVFrame *f)
{
   dv_render_t *r = &ctx->render;
   uint8_t *dst[4]  = { (uint8_t *)r->buf, NULL, NULL, NULL };
   int dst_stride[4] = { (int)(f->width * sizeof(uint32_t)), 0, 0, 0 };

   if (f->width <= 0 || f->height <= 0 ||
       f->width > DV_MAX_WIDTH || f->height > DV_MAX_HEIGHT)
      return false;

   if (!r->sws || f->width != r->sws_w || f->height != r->sws_h || f->format != r->sws_fmt)
   {
      sws_freeContext(r->sws);
      /* Same size in and out: the flag only picks chroma upsampling. */
      r->sws = sws_getContext(f->width, f->height, f->format,
                              f->width, f->height, DV_RGB_FMT,
                              SWS_BILINEAR | SWS_ACCURATE_RND, NULL, NULL, NULL);
      if (!r->sws)
         return false;
      r->sws_w   = f->width;
      r->sws_h   = f->height;
      r->sws_fmt = f->format;

      /* MPEG-2 on DVD is BT.601, limited range; output full-range RGB. */
      sws_setColorspaceDetails(r->sws,
                               sws_getCoefficients(SWS_CS_ITU601), 0,
                               sws_getCoefficients(SWS_CS_ITU601), 1,
                               0, 1 << 16, 1 << 16);
   }

   sws_scale(r->sws, (const uint8_t *const *)f->data, f->linesize,
             0, f->height, dst, dst_stride);

   r->width  = (unsigned)f->width;
   r->height = (unsigned)f->height;
   r->dar    = frame_dar(ctx, f);
   r->valid  = true;
   r->panscan_x = panscan_left(f);
   return true;
}

double dv_render_source_dar(const dv_ctx_t *ctx)
{
   return ctx->opt.aspect == 1 ? 4.0 / 3.0 :
          ctx->opt.aspect == 2 ? 16.0 / 9.0 : ctx->render.dar;
}

dv_disp_t dv_render_mode_for(const dv_ctx_t *ctx, double dar)
{
   dv_disp_t want;

   if (ctx->opt.tv_display == DV_TV_WIDE || dar < 1.5)
      return DV_DISP_NORMAL;
   want = ctx->opt.tv_display == DV_TV_PANSCAN ? DV_DISP_PANSCAN : DV_DISP_LETTERBOX;
   /* IFO permitted_df: 0 both, 1 pan&scan only, 2 letterbox only. */
   switch (ctx->nav.permitted_df)
   {
      case 1: return DV_DISP_PANSCAN;
      case 2: return DV_DISP_LETTERBOX;
      default: return want;
   }
}

dv_disp_t dv_render_mode(const dv_ctx_t *ctx)
{
   return dv_render_mode_for(ctx, dv_render_source_dar(ctx));
}

double dv_render_output_dar(const dv_ctx_t *ctx)
{
   return dv_render_mode(ctx) == DV_DISP_NORMAL ? dv_render_source_dar(ctx) : 4.0 / 3.0;
}

const uint32_t *dv_render_transform(dv_ctx_t *ctx, const uint32_t *src, dv_disp_t mode)
{
   dv_render_t *r = &ctx->render;
   unsigned w = r->width, h = r->height;
   const uint8_t *in[4] = { NULL, NULL, NULL, NULL };
   uint8_t *out[4] = { NULL, NULL, NULL, NULL };
   int in_stride[4] = { (int)(w * sizeof(uint32_t)), 0, 0, 0 };
   int out_stride[4] = { (int)(w * sizeof(uint32_t)), 0, 0, 0 };
   unsigned top = lb_top(h);
   int sw = mode == DV_DISP_PANSCAN ? (int)ps_width(w) : (int)w;
   int dh = mode == DV_DISP_LETTERBOX ? (int)(h - 2 * top) : (int)h;

   if (mode == DV_DISP_NORMAL)
      return src;

   if (!r->xsws || r->xsws_mode != (int)mode || r->xsws_w != w || r->xsws_h != h)
   {
      sws_freeContext(r->xsws);
      /* Area averaging for the letterbox squeeze (no line aliasing),
       * bicubic for the pan&scan stretch. */
      r->xsws = sws_getContext(sw, (int)h, DV_RGB_FMT, (int)w, dh, DV_RGB_FMT,
                               mode == DV_DISP_LETTERBOX ? SWS_AREA : SWS_BICUBIC,
                               NULL, NULL, NULL);
      r->xsws_mode = (int)mode;
      r->xsws_w = w;
      r->xsws_h = h;
      if (!r->xsws)
         return src;
      if (mode == DV_DISP_LETTERBOX)
         memset(r->xbuf, 0, (size_t)w * h * sizeof(uint32_t));
   }

   in[0]  = (const uint8_t *)(src + (mode == DV_DISP_PANSCAN ? r->panscan_x : 0));
   out[0] = (uint8_t *)(r->xbuf + (mode == DV_DISP_LETTERBOX ? (size_t)top * w : 0));
   sws_scale(r->xsws, in, in_stride, 0, (int)h, out, out_stride);
   return r->xbuf;
}

void dv_render_unmap(const dv_ctx_t *ctx, dv_disp_t mode, int *x, int *y)
{
   const dv_render_t *r = &ctx->render;
   int ref_h = (r->height == 576 || r->height == 288) ? 576 : 480;
   int top = (int)lb_top((unsigned)ref_h);

   if (mode == DV_DISP_LETTERBOX)
      *y = (*y - top) * ref_h / (ref_h - 2 * top);
   else if (mode == DV_DISP_PANSCAN)
   {
      /* Pan&scan offset in 720-wide button space. */
      int x0 = r->width ? r->panscan_x * 720 / (int)r->width : 0;
      *x = x0 + *x * (int)ps_width(720) / 720;
   }
}
