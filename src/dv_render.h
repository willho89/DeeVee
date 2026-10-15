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

typedef struct dv_render
{
   struct SwsContext *sws;
   int sws_w, sws_h, sws_fmt;  /* input config sws was built for */
   uint32_t *buf;              /* DV_MAX_WIDTH x DV_MAX_HEIGHT */
   unsigned  width, height;    /* of the last rendered picture */
   double    dar;              /* display aspect of the last picture */
   bool      valid;
} dv_render_t;

bool dv_render_init(struct dv_ctx *ctx);
void dv_render_free(struct dv_ctx *ctx);

/* Convert a decoded picture into ctx->render.buf. */
bool dv_render_frame(struct dv_ctx *ctx, const struct AVFrame *f);

#endif
