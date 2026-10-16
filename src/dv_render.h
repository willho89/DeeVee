/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Decoded picture -> XRGB8888 (CLAUDE.md §5.4).
 */
#ifndef DV_RENDER_H
#define DV_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#define DV_MAX_WIDTH  720
#define DV_MAX_HEIGHT 576

struct dv_ctx;
struct AVFrame;
struct SwsContext;

/* How a picture reaches the screen (M6b, D-025). Values double as the
 * libdvdnav spu stream mode (1 = letterbox, 2 = pan&scan). */
typedef enum
{
   DV_DISP_NORMAL    = 0,   /* as stored (4:3, or 16:9 on a 16:9 TV) */
   DV_DISP_LETTERBOX = 1,   /* 16:9 squeezed into a 4:3 frame with bars */
   DV_DISP_PANSCAN   = 2    /* 4:3 window cropped out of 16:9 */
} dv_disp_t;

typedef struct dv_render
{
   struct SwsContext *sws;
   int sws_w, sws_h, sws_fmt;  /* input config sws was built for */
   uint32_t *buf;              /* DV_MAX_WIDTH x DV_MAX_HEIGHT */
   unsigned  width, height;    /* of the last rendered picture */
   double    dar;              /* display aspect of the last picture */
   bool      valid;
   int       panscan_x;        /* pan&scan window left edge, stored-frame px */

   /* 4:3 output of 16:9 pictures (letterbox / pan&scan). */
   struct SwsContext *xsws;
   int       xsws_mode;
   unsigned  xsws_w, xsws_h;
   uint32_t *xbuf;             /* DV_MAX_WIDTH x DV_MAX_HEIGHT */
} dv_render_t;

bool dv_render_init(struct dv_ctx *ctx);
void dv_render_free(struct dv_ctx *ctx);

/* Convert a decoded picture into ctx->render.buf. */
bool dv_render_frame(struct dv_ctx *ctx, const struct AVFrame *f);

/* Display aspect of the last picture: the stream's, or the Aspect Ratio
 * override. */
double dv_render_source_dar(const struct dv_ctx *ctx);

/* Display mode for a picture of display aspect dar under the TV Display
 * Type option and the disc's permitted display modes. */
dv_disp_t dv_render_mode_for(const struct dv_ctx *ctx, double dar);

/* Mode of the last rendered picture. */
dv_disp_t dv_render_mode(const struct dv_ctx *ctx);

/* Aspect the frontend should show the output at. */
double dv_render_output_dar(const struct dv_ctx *ctx);

/* src (width x height, as stored) -> 4:3 picture in render.xbuf for the
 * given mode; returns render.xbuf. */
const uint32_t *dv_render_transform(struct dv_ctx *ctx, const uint32_t *src, dv_disp_t mode);

/* Map output coordinates back to stored-frame ones (720 x 480/576 space). */
void dv_render_unmap(const struct dv_ctx *ctx, dv_disp_t mode, int *x, int *y);

#endif
