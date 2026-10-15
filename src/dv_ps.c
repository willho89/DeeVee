/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PS/PES router (CLAUDE.md §5.3).
 *
 * Ported from VLC modules/demux/mpeg/pes.h (ExtractPESTimestamp,
 * ParsePESHeader), modules/demux/mpeg/ps.h (ps_pkt_size, ps_pkt_id,
 * ps_track_fill skip sizes) and modules/access/dvdnav.c DemuxBlock()
 * @ 843607a066be (3.0.x).
 *
 * Original code:
 *   Copyright (C) 2004-2015 VLC authors and VideoLAN
 *   Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *   LGPL-2.1-or-later
 * Adapted for DeeVee (combined work distributed under GPL-2.0-or-later).
 */
#include <string.h>

#include "dv_ctx.h"
#include "dv_ps.h"

/* Ported from VLC pes.h ExtractPESTimestamp(). */
static int extract_pes_timestamp(const uint8_t *p, uint8_t flags, int64_t *ret)
{
   /* check marker bits, and flags = b 0010, 0011 or 0001 */
   if ((p[0] & 0xC1) != 0x01 || (p[2] & 0x01) != 0x01 ||
       (p[4] & 0x01) != 0x01 || (p[0] & 0x30) == 0 ||
       (p[0] >> 5) > flags)
      return 0;

   *ret = ((int64_t)(p[0] & 0x0e) << 29) |
          (int64_t)(p[1] << 22) |
          ((int64_t)(p[2] & 0xfe) << 14) |
          (int64_t)(p[3] << 7) |
          (int64_t)(p[4] >> 1);
   return 1;
}

/* Ported from VLC ps.h ps_pkt_size(). Returns -1 if not enough data. */
static int pkt_size(const uint8_t *p, size_t len)
{
   if (len < 4)
      return -1;
   switch (p[3])
   {
      case 0xB9:  /* end of stream */
         return 4;
      case 0xBA:  /* pack header */
         if (len >= 14 && (p[4] >> 6) == 0x01)
            return 14 + (p[13] & 0x07);
         if (len >= 12 && (p[4] >> 4) == 0x02)
            return 12;
         return -1;
      default:
         if (len >= 6)
            return 6 + ((p[4] << 8) | p[5]);
         return -1;
   }
}

/* Ported from VLC pes.h ParsePESHeader(). Returns header size or -1. */
static int parse_pes_header(const uint8_t *p, size_t len, int64_t *pts)
{
   unsigned skip;

   *pts = DV_NOPTS;
   if (len < 9)
      return -1;

   if ((p[6] & 0xC0) == 0x80)
   {
      /* MPEG-2 PES */
      skip = p[8] + 9u;
      if ((p[7] & 0x80) && len >= 9 + 5)
         extract_pes_timestamp(&p[9], p[7] >> 6, pts);
   }
   else
   {
      /* MPEG-1 PES (VCD-style content on some DVDs) */
      skip = 6;
      while (skip < 23 && skip < len && p[skip] == 0xFF)
         skip++;
      if (skip >= 23 || skip >= len)
         return -1;
      if ((p[skip] & 0xC0) == 0x40)  /* STD buffer size */
         skip += 2;
      if (skip >= len)
         return -1;
      if (p[skip] & 0x20)
      {
         if (len >= skip + 5)
            extract_pes_timestamp(&p[skip], p[skip] >> 4, pts);
         skip += (p[skip] & 0x10) ? 10 : 5;
      }
      else
      {
         if (p[skip] != 0x0F)
            return -1;
         skip += 1;
      }
   }
   return skip <= len ? (int)skip : -1;
}

static void route(dv_ctx_t *ctx, unsigned id, const uint8_t *data, size_t len, int64_t pts)
{
   int64_t tl = dv_clock_to_timeline(&ctx->clock, pts);

   if (id == DV_PS_ID_VIDEO)
   {
      dv_clock_demuxed_video(&ctx->clock, tl);
      ctx->ps_stats.video_pes++;
      dv_video_push(ctx, data, len, tl);
   }
   else if (DV_PS_IS_MPA(id) || DV_PS_IS_AC3(id) || DV_PS_IS_DTS(id) || DV_PS_IS_LPCM(id))
   {
      ctx->ps_stats.audio_pes++;
      dv_audio_push(ctx, id, data, len, tl);
   }
   else if (DV_PS_IS_SPU(id))
      ctx->ps_stats.spu_pes++;     /* TODO(M3): dv_spu */
   else
      ctx->ps_stats.other_pes++;
}

/* Ported in structure from VLC dvdnav.c DemuxBlock(). */
void dv_ps_demux(dv_ctx_t *ctx, const uint8_t *p, size_t len)
{
   ctx->ps_stats.packs++;

   while (len >= 4)
   {
      int size;

      if (p[0] != 0 || p[1] != 0 || p[2] != 1)
      {
         ctx->ps_stats.errors++;
         return;
      }

      size = pkt_size(p, len);
      if (size <= 0 || (size_t)size > len)
      {
         ctx->ps_stats.errors++;
         return;
      }

      switch (p[3])
      {
         case 0xB9: /* end */
         case 0xBA: /* pack header: SCR unused, we clock off PTS */
         case 0xBB: /* system header */
         case 0xBC: /* PSM */
         case 0xBE: /* padding */
         case 0xBF: /* private stream 2: NAV (PCI/DSI), consumed by libdvdnav */
            break;

         default:
         {
            int64_t pts;
            int hdr = parse_pes_header(p, (size_t)size, &pts);
            unsigned id = p[3];
            const uint8_t *payload;
            size_t plen;

            if (hdr < 0)
            {
               ctx->ps_stats.errors++;
               break;
            }
            payload = p + hdr;
            plen    = (size_t)size - (size_t)hdr;

            if (id == 0xBD)
            {
               unsigned skip;
               if (plen < 1)
                  break;
               id = 0xBD00 | payload[0];
               /* Substream headers: id, frame count, first access unit
                * pointer (2) for audio (VLC ps.h ps_track_fill(); FFmpeg
                * mpeg.c). LPCM's own 3-byte header stays: pcm_dvd parses it.
                * SPU: id only. */
               if (DV_PS_IS_AC3(id) || DV_PS_IS_DTS(id) || DV_PS_IS_LPCM(id))
                  skip = 4;
               else
                  skip = 1;
               if (plen < skip)
                  break;
               payload += skip;
               plen    -= skip;
            }
            route(ctx, id, payload, plen, pts);
            break;
         }
      }

      p   += size;
      len -= (size_t)size;
   }
}
