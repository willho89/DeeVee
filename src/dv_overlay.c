/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CLUT -> RGB and subpicture blending (CLAUDE.md §5.5). Subpicture
 * coordinates live in the 720-wide stored-frame space even for 704/352-wide
 * video, so positions are scaled to the decoded picture.
 */
#include <string.h>

#include "dv_ctx.h"
#include "dv_overlay.h"
#include "dv_spu.h"

static uint8_t clamp8(int v)
{
   return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601 limited-range YCbCr -> full-range RGB, as the video path. */
static uint32_t ycrcb_to_rgb(uint32_t e)
{
   int y  = (int)((e >> 16) & 0xFF) - 16;
   int cr = (int)((e >> 8) & 0xFF) - 128;
   int cb = (int)(e & 0xFF) - 128;
   int r  = (298 * y + 409 * cr + 128) >> 8;
   int g  = (298 * y - 100 * cb - 208 * cr + 128) >> 8;
   int b  = (298 * y + 516 * cb + 128) >> 8;
   return ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) | clamp8(b);
}

void dv_overlay_init(dv_ctx_t *ctx)
{
   static const uint32_t grey[16] = {
      /* Until the disc sends a CLUT: a neutral ramp so something shows. */
      0x108080, 0x808080, 0xEB8080, 0x408080, 0x108080, 0x808080, 0xEB8080, 0x408080,
      0x108080, 0x808080, 0xEB8080, 0x408080, 0x108080, 0x808080, 0xEB8080, 0x408080,
   };
   memset(&ctx->overlay, 0, sizeof(ctx->overlay));
   dv_overlay_set_clut(ctx, grey);
   ctx->overlay.have_clut = false;
}

void dv_overlay_set_clut(dv_ctx_t *ctx, const uint32_t clut[16])
{
   dv_overlay_t *o = &ctx->overlay;
   int i;
   for (i = 0; i < 16; i++)
   {
      o->clut[i] = clut[i];
      o->rgb[i]  = ycrcb_to_rgb(clut[i]);
   }
   o->have_clut = true;
}

static uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
   unsigned ia = 255 - a;
   unsigned r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia + 127) / 255;
   unsigned g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia + 127) / 255;
   unsigned b = ((src & 0xFF) * a + (dst & 0xFF) * ia + 127) / 255;
   return (r << 16) | (g << 8) | b;
}

void dv_overlay_compose(dv_ctx_t *ctx, const uint32_t *base, uint32_t *out,
                        unsigned w, unsigned h, const dv_subpic_t *sp,
                        const dv_highlight_t *hl)
{
   const dv_overlay_t *o = &ctx->overlay;
   unsigned ref_h = (h == 288 || h == 576) ? 576 : 480;
   uint32_t col[4], hcol[4];
   unsigned alp[4], halp[4];
   int i, dx0, dx1, dy0, dy1, dx, dy;

   if (out != base)
      memcpy(out, base, (size_t)w * h * sizeof(uint32_t));
   if (!sp || !sp->pix)
      return;

   for (i = 0; i < 4; i++)
   {
      col[i] = o->rgb[sp->color[i] & 0x0F];
      alp[i] = sp->alpha[i] * 17u;   /* 0..15 -> 0..255 */
      if (hl && hl->active)
      {
         hcol[i] = o->rgb[hl->color[i] & 0x0F];
         halp[i] = hl->alpha[i] * 17u;
      }
   }

   /* Destination rectangle, scaled from 720 x ref_h stored-frame space. */
   dx0 = (int)((int64_t)sp->x * w / 720);
   dx1 = (int)((int64_t)(sp->x + sp->w) * w / 720);
   dy0 = (int)((int64_t)sp->y * h / ref_h);
   dy1 = (int)((int64_t)(sp->y + sp->h) * h / ref_h);
   if (dx1 > (int)w) dx1 = (int)w;
   if (dy1 > (int)h) dy1 = (int)h;

   for (dy = dy0 < 0 ? 0 : dy0; dy < dy1; dy++)
   {
      int sy = (int)((int64_t)dy * ref_h / h);    /* stored-frame line */
      const uint8_t *src = sp->pix + (size_t)(sy - sp->y) * (size_t)sp->w;
      uint32_t *row = out + (size_t)dy * w;
      if (sy < sp->y || sy >= sp->y + sp->h)
         continue;
      for (dx = dx0 < 0 ? 0 : dx0; dx < dx1; dx++)
      {
         int sx = (int)((int64_t)dx * 720 / w);    /* stored-frame column */
         unsigned v, a;
         uint32_t c;
         if (sx < sp->x || sx >= sp->x + sp->w)
            continue;
         v = src[sx - sp->x] & 3;
         if (hl && hl->active && sx >= hl->sx && sx <= hl->ex && sy >= hl->sy && sy <= hl->ey)
         {
            a = halp[v];
            c = hcol[v];
         }
         else
         {
            a = alp[v];
            c = col[v];
         }
         if (a == 0)
            continue;
         row[dx] = a == 255 ? c : blend(row[dx], c, a);
      }
   }
}
