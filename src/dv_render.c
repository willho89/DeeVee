/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * yuv420p -> XRGB8888 via swscale, BT.601 limited range (CLAUDE.md §5.4).
 */
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "dv_ctx.h"
#include "dv_render.h"

/* libretro XRGB8888 is a native-endian 0x00RRGGBB uint32. */
#define DV_RGB_FMT AV_PIX_FMT_0RGB32

bool dv_render_init(dv_ctx_t *ctx)
{
   dv_render_t *r = &ctx->render;
   memset(r, 0, sizeof(*r));
   r->buf = calloc((size_t)DV_MAX_WIDTH * DV_MAX_HEIGHT, sizeof(uint32_t));
   r->width  = 720;
   r->height = 480;
   r->dar    = 4.0 / 3.0;
   return r->buf != NULL;
}

void dv_render_free(dv_ctx_t *ctx)
{
   dv_render_t *r = &ctx->render;
   sws_freeContext(r->sws);
   r->sws = NULL;
   free(r->buf);
   r->buf = NULL;
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
   return true;
}
