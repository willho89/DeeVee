/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CLUT handling and subpicture compositing onto the XRGB8888 frame
 * (CLAUDE.md §5.5).
 */
#ifndef DV_OVERLAY_H
#define DV_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

struct dv_ctx;
struct dv_subpic;

/* Menu button highlight (M4): inside the rectangle, these colours/alphas
 * replace the subpicture's own. */
typedef struct dv_highlight
{
   bool    active;
   int     sx, sy, ex, ey;     /* inclusive, stored-frame coordinates */
   uint8_t color[4];           /* CLUT indices */
   uint8_t alpha[4];           /* 0..15 */
} dv_highlight_t;

typedef struct dv_overlay
{
   uint32_t clut[16];          /* as delivered: 0x00YYCrCb */
   uint32_t rgb[16];           /* 0x00RRGGBB */
   bool     have_clut;
} dv_overlay_t;

void dv_overlay_init(struct dv_ctx *ctx);

/* SPU_CLUT_CHANGE: 16 entries of 0x00YYCrCb. */
void dv_overlay_set_clut(struct dv_ctx *ctx, const uint32_t clut[16]);

/* out = base with the subpicture (and optional highlight) blended on top.
 * base/out are w x h, packed (pitch = w). */
void dv_overlay_compose(struct dv_ctx *ctx, const uint32_t *base, uint32_t *out,
                        unsigned w, unsigned h, const struct dv_subpic *sp,
                        const dv_highlight_t *hl);

#endif
