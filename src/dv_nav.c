/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libdvdnav open sequence + event loop (CLAUDE.md §5.2).
 *
 * Open/config sequence follows VLC modules/access/dvdnav.c Open() and
 * libdvdnav examples/menus.c. The event handling is ported from VLC
 * modules/access/dvdnav.c Demux() @ 843607a066be (3.0.x).
 *
 * Original VLC code:
 *   Copyright (C) 2004-2009 VLC authors and VideoLAN
 *   Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *   LGPL-2.1-or-later
 * Adapted for DeeVee (combined work distributed under GPL-2.0-or-later).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_types.h>

#include "dv_ctx.h"
#include "dv_nav.h"

/* ---- logging bridge --------------------------------------------------- */

/* libdvdnav/libdvdread hand the same 'priv' to the logger and to the stream
 * callbacks, so priv is always a dv_stream_t* (its ctx is all we need here;
 * for directory sources the stream is otherwise empty). */
static void nav_log(void *priv, dvdnav_logger_level_t level, const char *fmt, va_list va)
{
   dv_ctx_t *ctx = ((dv_stream_t *)priv)->ctx;
   char buf[1024];
   enum retro_log_level rl;

   switch (level)
   {
      case DVDNAV_LOGGER_LEVEL_ERROR: rl = RETRO_LOG_ERROR; break;
      case DVDNAV_LOGGER_LEVEL_WARN:  rl = RETRO_LOG_WARN;  break;
      case DVDNAV_LOGGER_LEVEL_INFO:  rl = RETRO_LOG_INFO;  break;
      default:                        rl = RETRO_LOG_DEBUG; break;
   }
   vsnprintf(buf, sizeof(buf), fmt, va);
   dv_log(ctx, rl, "[dvdnav] %s\n", buf);
}

static void read_log(void *priv, dvd_logger_level_t level, const char *fmt, va_list va)
{
   /* dvd_logger_level_t and dvdnav_logger_level_t share values. */
   nav_log(priv, (dvdnav_logger_level_t)level, fmt, va);
}

/* ---- open / close ----------------------------------------------------- */

/* Pick the VM region (§5.8): lowest region the disc allows. Setting the VM's
 * region from the disc mask defeats RCE checks in the disc's VM program. */
static int pick_region(int32_t disc_mask)
{
   int r;
   for (r = 1; r <= 8; r++)
      if (disc_mask & (1 << (r - 1)))
         return r;
   return 1;  /* mask 0: shouldn't happen (region 0 discs report 0xFF) */
}

bool dv_nav_open(dv_ctx_t *ctx, const dv_source_t *src)
{
   dv_nav_t *nav = &ctx->nav;
   dvdnav_logger_cb logcb = { nav_log };
   dvdnav_status_t st;

   memset(nav, 0, sizeof(*nav));
   nav->stream.ctx    = ctx;
   nav->policy_title  = -1;

   if (src->kind == DV_SOURCE_IMAGE)
   {
      if (!dv_stream_open(ctx, src, &nav->stream))
         return false;
      st = dvdnav_open_stream2(&nav->dvdnav, &nav->stream, &logcb, dv_stream_callbacks());
   }
   else
      st = dvdnav_open2(&nav->dvdnav, &nav->stream, &logcb, src->path);

   if (st != DVDNAV_STATUS_OK || !nav->dvdnav)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "dvdnav: failed to open %s\n", src->path);
      dv_stream_close(&nav->stream);
      nav->dvdnav = NULL;
      return false;
   }

   if (dvdnav_set_readahead_flag(nav->dvdnav, 1) != DVDNAV_STATUS_OK)
      dv_log(ctx, RETRO_LOG_WARN, "dvdnav: set_readahead_flag: %s\n",
             dvdnav_err_to_string(nav->dvdnav));

   /* Chapter/time positioning relative to the PGC, as VLC does. */
   if (dvdnav_set_PGC_positioning_flag(nav->dvdnav, 1) != DVDNAV_STATUS_OK)
      dv_log(ctx, RETRO_LOG_WARN, "dvdnav: set_PGC_positioning_flag: %s\n",
             dvdnav_err_to_string(nav->dvdnav));

   /* TODO(M5): menu/audio/SPU language + forced region from core options. */
   dvdnav_menu_language_select(nav->dvdnav, "en");
   dvdnav_audio_language_select(nav->dvdnav, "en");
   dvdnav_spu_language_select(nav->dvdnav, ctx->opt.sub_lang[0] ? ctx->opt.sub_lang : "en");

   if (dvdnav_get_disk_region_mask(nav->dvdnav, &nav->disc_region_mask) != DVDNAV_STATUS_OK)
      nav->disc_region_mask = 0xFF;
   nav->vm_region = pick_region(nav->disc_region_mask);
   dvdnav_set_region_mask(nav->dvdnav, 1 << (nav->vm_region - 1));

   return true;
}

void dv_nav_close(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   if (nav->dvdnav)
      dvdnav_close(nav->dvdnav);
   nav->dvdnav = NULL;
   dv_stream_close(&nav->stream);
}

/* ---- disc info -------------------------------------------------------- */

static void format_region_mask(int32_t mask, char *out, size_t len)
{
   size_t pos = 0;
   int r;
   out[0] = '\0';
   for (r = 1; r <= 8; r++)
      if (mask & (1 << (r - 1)))
         pos += (size_t)snprintf(out + pos, pos < len ? len - pos : 0, "%s%d", pos ? "," : "", r);
   if (!pos)
      snprintf(out, len, "none");
}

/* libdvdnav doesn't expose the VTS count, so read the VMG IFO through a
 * separate, short-lived libdvdread handle. */
static int probe_vts_count(dv_ctx_t *ctx, const dv_source_t *src)
{
   dvd_logger_cb logcb = { read_log };
   dvd_reader_t *dvd;
   ifo_handle_t *vmg;
   dv_stream_t stream;
   int count = -1;

   if (src->kind == DV_SOURCE_IMAGE)
   {
      if (!dv_stream_open(ctx, src, &stream))
         return -1;
      dvd = DVDOpenStream2(&stream, &logcb, dv_stream_callbacks());
   }
   else
   {
      memset(&stream, 0, sizeof(stream));
      stream.ctx = ctx;
      dvd = DVDOpen2(&stream, &logcb, src->path);
   }

   if (dvd)
   {
      vmg = ifoOpen(dvd, 0);
      if (vmg && vmg->vmgi_mat)
         count = vmg->vmgi_mat->vmg_nr_of_title_sets;
      if (vmg)
         ifoClose(vmg);
      DVDClose(dvd);
   }
   dv_stream_close(&stream);
   return count;
}

void dv_nav_log_disc_info(dv_ctx_t *ctx, const dv_source_t *src)
{
   dv_nav_t *nav = &ctx->nav;
   const char *title = NULL, *serial = NULL;
   char *volid;
   int32_t titles = 0;
   char regions[32];

   volid = dvdnav_get_volid_string(nav->dvdnav);
   dvdnav_get_title_string(nav->dvdnav, &title);
   dvdnav_get_serial_string(nav->dvdnav, &serial);
   dvdnav_get_number_of_titles(nav->dvdnav, &titles);
   format_region_mask(nav->disc_region_mask, regions, sizeof(regions));

   dv_log(ctx, RETRO_LOG_INFO, "Disc volume ID : %s\n", volid ? volid : "(unknown)");
   dv_log(ctx, RETRO_LOG_INFO, "Disc title     : %s\n", title && *title ? title : "(none)");
   dv_log(ctx, RETRO_LOG_INFO, "Disc serial    : %s\n", serial && *serial ? serial : "(none)");
   dv_log(ctx, RETRO_LOG_INFO, "Region mask    : 0x%02X (regions %s); VM region set to %d\n",
          (unsigned)nav->disc_region_mask & 0xFF, regions, nav->vm_region);
   dv_log(ctx, RETRO_LOG_INFO, "Titles         : %d\n", (int)titles);
   dv_log(ctx, RETRO_LOG_INFO, "Title sets     : %d\n", probe_vts_count(ctx, src));

   free(volid);
}

/* ---- event loop ------------------------------------------------------- */

static const char *event_name(int event)
{
   switch (event)
   {
      case DVDNAV_BLOCK_OK:            return "BLOCK_OK";
      case DVDNAV_NOP:                 return "NOP";
      case DVDNAV_STILL_FRAME:         return "STILL_FRAME";
      case DVDNAV_SPU_STREAM_CHANGE:   return "SPU_STREAM_CHANGE";
      case DVDNAV_AUDIO_STREAM_CHANGE: return "AUDIO_STREAM_CHANGE";
      case DVDNAV_VTS_CHANGE:          return "VTS_CHANGE";
      case DVDNAV_CELL_CHANGE:         return "CELL_CHANGE";
      case DVDNAV_NAV_PACKET:          return "NAV_PACKET";
      case DVDNAV_STOP:                return "STOP";
      case DVDNAV_HIGHLIGHT:           return "HIGHLIGHT";
      case DVDNAV_SPU_CLUT_CHANGE:     return "SPU_CLUT_CHANGE";
      case DVDNAV_HOP_CHANNEL:         return "HOP_CHANNEL";
      case DVDNAV_WAIT:                return "WAIT";
      default:                         return "UNKNOWN";
   }
}

/* Read-ahead targets. DVD muxes video up to ~1 s ahead of audio with the
 * same PTS, so both streams' demux horizons must reach past the clock. */
#define DV_READ_LEAD_MIN (90000 * 3 / 10)   /* 0.3 s */
#define DV_READ_LEAD_MAX (90000 * 3)        /* 3 s hard cap */

static bool need_more(dv_ctx_t *ctx)
{
   const dv_nav_t *nav = &ctx->nav;
   const dv_video_t *v = &ctx->video;
   const dv_clock_t *c = &ctx->clock;
   double need, cap;
   bool video_ok, audio_ok;

   if (nav->still || nav->wait || nav->stopped)
      return false;
   if (dv_video_fifo_full(v))
      return false;
   if (!c->anchored)
      return v->qcount == 0;   /* until there's a picture to anchor on */

   need = c->now + DV_READ_LEAD_MIN;
   cap  = c->now + DV_READ_LEAD_MAX;

   video_ok = c->video_max != DV_NOPTS && c->video_max >= need;
   /* No audio for the selected stream (yet / in this content): don't wait
    * for it, the cap bounds the read-ahead. */
   audio_ok = c->audio_max == DV_NOPTS || c->audio_max >= need;
   if (video_ok && audio_ok)
      return false;

   /* A still picture with minutes of audio, or a stream missing here: keep
    * real-time pacing instead of reading the whole cell in one go. */
   if ((c->video_max != DV_NOPTS && c->video_max > cap) ||
       (c->audio_max != DV_NOPTS && c->audio_max > cap))
      return false;
   return true;
}

static void end_still(dv_ctx_t *ctx, bool skip)
{
   dv_nav_t *nav = &ctx->nav;
   if (skip)
      dvdnav_still_skip(nav->dvdnav);
   nav->still = false;
   dv_clock_unanchor(&ctx->clock);
}

static void on_nav_packet(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   pci_t *pci = dvdnav_get_current_nav_pci(nav->dvdnav);

   nav->vobu_index = 1;
   nav->vobu_flush = 0;
   if (!pci)
      return;

   {
      int64_t prev = ctx->clock.prev_e_ptm;
      if (dv_clock_vobu(&ctx->clock, pci->pci_gi.vobu_s_ptm, pci->pci_gi.vobu_e_ptm))
      {
         nav->discontinuities++;
         dv_log(ctx, RETRO_LOG_DEBUG, "vobu discontinuity: s_ptm %u, prev e_ptm %lld -> offset %lld\n",
                pci->pci_gi.vobu_s_ptm, (long long)prev, (long long)ctx->clock.offset);
      }
   }

   /* Remember this PCI with its presentation time (see dv_nav_presented_pci). */
   {
      dv_pci_slot_t *slot;
      if (nav->pci_count == DV_PCI_QUEUE)
      {
         nav->pci_head = (nav->pci_head + 1) % DV_PCI_QUEUE;
         nav->pci_count--;
      }
      slot = &nav->pcis[(nav->pci_head + nav->pci_count) % DV_PCI_QUEUE];
      slot->start = dv_clock_to_timeline(&ctx->clock, pci->pci_gi.vobu_s_ptm);
      slot->pci   = *pci;
      nav->pci_count++;
   }

   /* Ported from VLC dvdnav.c DVDNAV_NAV_PACKET: a VOBU that ends its
    * sequence early (vobu_se_e_ptm) holds a still picture mid-VOBU; force it
    * out once the last block of its reference picture has been read. */
   if (pci->pci_gi.vobu_se_e_ptm != 0 &&
       pci->pci_gi.vobu_se_e_ptm < pci->pci_gi.vobu_e_ptm)
   {
      dsi_t *dsi = dvdnav_get_current_nav_dsi(nav->dvdnav);
      if (dsi)
      {
         if (dsi->dsi_gi.vobu_3rdref_ea)
            nav->vobu_flush = dsi->dsi_gi.vobu_3rdref_ea;
         else if (dsi->dsi_gi.vobu_2ndref_ea)
            nav->vobu_flush = dsi->dsi_gi.vobu_2ndref_ea;
         else if (dsi->dsi_gi.vobu_1stref_ea)
            nav->vobu_flush = dsi->dsi_gi.vobu_1stref_ea;
      }
   }
}

/* Ported from VLC modules/access/dvdnav.c Demux() event switch
 * @ 843607a066be, on the libdvdnav examples/menus.c loop skeleton. */
void dv_nav_fill(dv_ctx_t *ctx, int max_blocks)
{
   dv_nav_t *nav = &ctx->nav;
   int i;

   if (!nav->dvdnav)
      return;

   for (i = 0; i < max_blocks; i++)
   {
      /* Event payloads (highlight, VTS change, ...) are written into the
       * buffer we pass; only BLOCK_OK repoints buf into libdvdnav's cache. */
      uint8_t mem[DVD_VIDEO_LB_LEN];
      uint8_t *buf = mem;
      int32_t event = 0, len = 0;

      /* While looking for a picture to anchor on, decode as we go. */
      if (!ctx->clock.anchored)
         dv_video_decode_ahead(ctx);
      if (!need_more(ctx))
         break;

      if (dvdnav_get_next_cache_block(nav->dvdnav, &buf, &event, &len) != DVDNAV_STATUS_OK)
      {
         dv_log(ctx, RETRO_LOG_ERROR, "dvdnav: get_next_block: %s\n",
                dvdnav_err_to_string(nav->dvdnav));
         nav->stopped = true;
         return;
      }

      if (event != DVDNAV_BLOCK_OK && event != DVDNAV_NAV_PACKET && event != DVDNAV_NOP)
         dv_log(ctx, RETRO_LOG_DEBUG, "event %s\n", event_name(event));

      switch (event)
      {
         case DVDNAV_BLOCK_OK:
            nav->blocks++;
            dv_ps_demux(ctx, buf, (size_t)len);
            if (nav->vobu_index > 0)
            {
               if (nav->vobu_flush == nav->vobu_index)
                  dv_video_force_still(ctx);
               nav->vobu_index++;
            }
            break;

         case DVDNAV_NAV_PACKET:
            on_nav_packet(ctx);
            dv_ps_demux(ctx, buf, (size_t)len);
            break;

         case DVDNAV_STILL_FRAME:
         {
            dvdnav_still_event_t *ev = (dvdnav_still_event_t *)buf;
            /* Emit the held picture; the still timer starts once it's on
             * screen (dv_nav_service). */
            nav->still            = true;
            nav->still_infinite   = ev->length == 0xFF;
            nav->still_length     = (unsigned)ev->length;
            nav->still_ticks_left = -1;
            nav->stills++;
            dv_log(ctx, RETRO_LOG_DEBUG, "still: %s%u s\n",
                   nav->still_infinite ? "infinite / " : "", nav->still_length);
            dv_video_force_still(ctx);
            break;
         }

         case DVDNAV_WAIT:
            nav->wait = true;
            break;

         case DVDNAV_VTS_CHANGE:
         {
            dvdnav_vts_change_event_t *ev = (dvdnav_vts_change_event_t *)buf;
            uint32_t w = 0, h = 0;
            dvdnav_get_video_resolution(nav->dvdnav, &w, &h);
            nav->ifo_aspect = dvdnav_get_video_aspect(nav->dvdnav);
            dv_spu_clear(ctx);
            dv_log(ctx, RETRO_LOG_INFO, "VTS %d -> %d (domain %d), video %ux%u, aspect code %u\n",
                   ev->old_vtsN, ev->new_vtsN, ev->new_domain, w, h, nav->ifo_aspect);
            break;
         }

         case DVDNAV_CELL_CHANGE:
         {
            int32_t title = 0, part = 0;
            if (dvdnav_current_title_info(nav->dvdnav, &title, &part) == DVDNAV_STATUS_OK)
               dv_log(ctx, RETRO_LOG_DEBUG, "cell change: title %d chapter %d\n", title, part);
            break;
         }

         case DVDNAV_SPU_CLUT_CHANGE:
            /* 16 x uint32 0x00YYCrCb in the event buffer. */
            dv_overlay_set_clut(ctx, (const uint32_t *)buf);
            ctx->spu.changed = true;
            break;

         case DVDNAV_SPU_STREAM_CHANGE:
            /* Physical stream for the current aspect; 0x80 = display off
             * (forced only). Also fires at every cell start. */
            dv_nav_apply_subtitle_options(ctx, false);
            dv_spu_select(ctx, dvdnav_get_active_spu_stream(nav->dvdnav));
            break;

         case DVDNAV_HOP_CHANNEL:
            /* As VLC: reset the clock, keep decoders (flushing would drop
             * the tail of every looping menu). Seeks flush explicitly. */
            dv_spu_clear(ctx);
            nav->hops++;
            nav->vobu_index = 0;
            nav->vobu_flush = 0;
            dv_clock_hop(&ctx->clock);
            break;

         case DVDNAV_STOP:
            dv_log(ctx, RETRO_LOG_INFO, "dvdnav: STOP (end of disc)\n");
            nav->stopped = true;
            dv_video_force_still(ctx);
            break;

         case DVDNAV_AUDIO_STREAM_CHANGE:
            /* Fires at every cell start; dv_audio_select ignores no-ops.
             * get_active_audio_stream() returns the physical stream (VM
             * maps SPRM 1 through the PGC audio control table). */
            dv_audio_select(ctx, dvdnav_get_active_audio_stream(nav->dvdnav));
            break;

         default:
            /* HIGHLIGHT, SPU_CLUT_CHANGE, SPU_STREAM_CHANGE: M3/M4. */
            break;
      }

      if (buf != mem)
         dvdnav_free_cache_block(nav->dvdnav, buf);
   }
}

void dv_nav_service(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;

   /* Like VLC's ES_OUT_GET_EMPTY: everything demuxed has been presented. */
   bool drained = dv_video_empty(&ctx->video) && dv_clock_drained(&ctx->clock);

   if (nav->still && drained)
   {
      if (nav->still_ticks_left < 0)
         nav->still_ticks_left = (int)(nav->still_length * ctx->clock.fps + 0.5);
      else if (!nav->still_infinite && --nav->still_ticks_left <= 0)
         end_still(ctx, true);
   }

   if (nav->wait && drained)
   {
      dvdnav_wait_skip(nav->dvdnav);
      nav->wait = false;
      nav->waits++;
      dv_clock_unanchor(&ctx->clock);
   }
}

void dv_nav_apply_subtitle_options(dv_ctx_t *ctx, bool force)
{
   dv_nav_t *nav = &ctx->nav;
   int32_t title = 0, part = 0;
   const dv_options_t *o = &ctx->opt;

   if (!nav->dvdnav || !dvdnav_is_domain_vts(nav->dvdnav))
      return;
   if (dvdnav_current_title_info(nav->dvdnav, &title, &part) != DVDNAV_STATUS_OK)
      return;
   if (!force && title == nav->policy_title)
      return;
   nav->policy_title = title;

   if (o->sub_lang[0])
   {
      uint16_t want = (uint16_t)((o->sub_lang[0] << 8) | o->sub_lang[1]);
      int logical;
      for (logical = 0; logical < 32; logical++)
         if (dvdnav_spu_stream_to_lang(nav->dvdnav, (uint8_t)logical) == want)
         {
            if (dvdnav_set_active_stream(nav->dvdnav, (uint8_t)logical,
                                         DVD_SUBTITLE_STREAM) == DVDNAV_STATUS_OK)
               dv_log(ctx, RETRO_LOG_INFO, "Subtitles: title %d, '%s' = logical stream %d\n",
                      title, o->sub_lang, logical);
            break;
         }
      if (logical == 32)
         dv_log(ctx, RETRO_LOG_INFO, "Subtitles: title %d has no '%s' stream\n", title, o->sub_lang);
   }

   if (o->subtitles == DV_SUBS_ON)
      dvdnav_toggle_spu_stream(nav->dvdnav, 1);
   else if (o->subtitles == DV_SUBS_OFF)
      dvdnav_toggle_spu_stream(nav->dvdnav, 0);

   dv_spu_select(ctx, dvdnav_get_active_spu_stream(nav->dvdnav));
}

/* ---- menus and input (CLAUDE.md §5.5 highlight, §5.9 input) ------------ */

const pci_t *dv_nav_presented_pci(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   const dv_clock_t *c = &ctx->clock;
   double now = c->now + c->tick * 0.5;

   if (!nav->pci_count)
      return NULL;
   /* Drop PCIs superseded by a later one that is already on screen. */
   while (c->anchored && nav->pci_count >= 2 &&
          (double)nav->pcis[(nav->pci_head + 1) % DV_PCI_QUEUE].start <= now)
   {
      nav->pci_head = (nav->pci_head + 1) % DV_PCI_QUEUE;
      nav->pci_count--;
   }
   return &nav->pcis[nav->pci_head].pci;
}

bool dv_nav_has_buttons(dv_ctx_t *ctx)
{
   const pci_t *pci = dv_nav_presented_pci(ctx);
   return pci && pci->hli.hl_gi.hli_ss != 0 && pci->hli.hl_gi.btn_ns > 0;
}

/* Ported from VLC modules/access/dvdnav.c ButtonUpdate() @ 843607a066be.
 * Differences: uses the PCI on screen rather than the one just read, and
 * isn't limited to menu domains (titles may carry buttons too). Rendering
 * follows the DVD model (highlight palette inside the button rectangle,
 * the subpicture's own palette elsewhere) rather than VLC's crop; see
 * D-017. */
void dv_nav_update_highlight(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   dv_highlight_t hl;
   int32_t button = 0;
   dvdnav_highlight_area_t area;
   const pci_t *pci;

   memset(&hl, 0, sizeof(hl));

   if (nav->dvdnav && dv_nav_has_buttons(ctx) &&
       dvdnav_get_current_highlight(nav->dvdnav, &button) == DVDNAV_STATUS_OK &&
       button > 0)
   {
      pci = dv_nav_presented_pci(ctx);
      if (dvdnav_get_highlight_area((pci_t *)pci, button, 0, &area) == DVDNAV_STATUS_OK)
      {
         int i;
         hl.active = true;
         hl.sx = (int)area.sx; hl.ex = (int)area.ex;
         hl.sy = (int)area.sy; hl.ey = (int)area.ey;
         for (i = 0; i < 4; i++)
         {
            hl.color[i] = (uint8_t)((area.palette >> (16 + 4 * i)) & 0x0F);
            hl.alpha[i] = (uint8_t)((area.palette >> (4 * i)) & 0x0F);
         }
      }
   }

   if (memcmp(&hl, &ctx->highlight, sizeof(hl)))
   {
      if (hl.active)
         dv_log(ctx, RETRO_LOG_DEBUG, "highlight: button %d (%d,%d)-(%d,%d)\n",
                button, hl.sx, hl.sy, hl.ex, hl.ey);
      ctx->highlight   = hl;
      ctx->spu.changed = true;   /* palette swap only, no re-decode */
   }
}

/* A user-initiated jump: up to 3 s of the old position is still queued, so
 * drop it all and show the new content as soon as it decodes. */
static void user_jump(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;

   if (nav->wait)
      dvdnav_wait_skip(nav->dvdnav);
   nav->wait  = false;
   nav->still = false;
   nav->stopped = false;
   nav->vobu_index = nav->vobu_flush = 0;
   nav->pci_count = 0;

   dv_video_flush(ctx);
   dv_audio_flush(ctx);
   dv_spu_clear(ctx);
   dv_clock_flush(&ctx->clock);
   memset(&ctx->highlight, 0, sizeof(ctx->highlight));
}

void dv_nav_button_move(dv_ctx_t *ctx, dv_button_dir_t dir)
{
   dv_nav_t *nav = &ctx->nav;
   pci_t *pci = (pci_t *)dv_nav_presented_pci(ctx);
   dvdnav_status_t st = DVDNAV_STATUS_ERR;
   int32_t before = 0, after = 0;

   if (!nav->dvdnav || !dv_nav_has_buttons(ctx))
      return;
   dvdnav_get_current_highlight(nav->dvdnav, &before);
   switch (dir)
   {
      case DV_BTN_UP:    st = dvdnav_upper_button_select(nav->dvdnav, pci); break;
      case DV_BTN_DOWN:  st = dvdnav_lower_button_select(nav->dvdnav, pci); break;
      case DV_BTN_LEFT:  st = dvdnav_left_button_select(nav->dvdnav, pci);  break;
      case DV_BTN_RIGHT: st = dvdnav_right_button_select(nav->dvdnav, pci); break;
   }
   dvdnav_get_current_highlight(nav->dvdnav, &after);
   dv_log(ctx, RETRO_LOG_DEBUG, "button move %d: %d -> %d%s\n", (int)dir, before, after,
          st == DVDNAV_STATUS_OK ? "" : " (refused)");
   /* Auto-action buttons activate on selection inside libdvdnav: if the VM
    * jumped, the position changed under us. */
   if (st == DVDNAV_STATUS_OK && pci->hli.btnit[after > 0 ? after - 1 : 0].auto_action_mode)
      user_jump(ctx);
}

static bool still_skip_if_still(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   if (!nav->still)
      return false;
   dvdnav_still_skip(nav->dvdnav);
   nav->still = false;
   dv_clock_unanchor(&ctx->clock);
   return true;
}

void dv_nav_action(dv_ctx_t *ctx, dv_action_t act)
{
   dv_nav_t *nav = &ctx->nav;
   pci_t *pci = (pci_t *)dv_nav_presented_pci(ctx);
   dvdnav_status_t st = DVDNAV_STATUS_ERR;
   const char *name = "";

   if (!nav->dvdnav)
      return;

   switch (act)
   {
      case DV_ACT_ACTIVATE:
         name = "activate";
         if (dv_nav_has_buttons(ctx))
            st = dvdnav_button_activate(nav->dvdnav, pci);
         else if (still_skip_if_still(ctx))
            return;
         break;
      case DV_ACT_GO_UP:
         name = "return";
         st = dvdnav_go_up(nav->dvdnav);
         break;
      case DV_ACT_ROOT_MENU:
         name = "root menu";
         st = dvdnav_menu_call(nav->dvdnav, DVD_MENU_Root);
         if (st != DVDNAV_STATUS_OK)
            st = dvdnav_menu_call(nav->dvdnav, DVD_MENU_Title);
         break;
      case DV_ACT_TITLE_MENU:
         name = "title menu";
         st = dvdnav_menu_call(nav->dvdnav, DVD_MENU_Title);
         if (st != DVDNAV_STATUS_OK)
            st = dvdnav_menu_call(nav->dvdnav, DVD_MENU_Root);
         break;
      case DV_ACT_PREV_CHAPTER:
         name = "previous chapter";
         if (dvdnav_is_domain_vts(nav->dvdnav))
            st = dvdnav_prev_pg_search(nav->dvdnav);
         break;
      case DV_ACT_NEXT_CHAPTER:
         name = "next chapter";
         if (dvdnav_is_domain_vts(nav->dvdnav))
            st = dvdnav_next_pg_search(nav->dvdnav);
         break;
   }

   dv_log(ctx, RETRO_LOG_DEBUG, "action %s: %s\n", name,
          st == DVDNAV_STATUS_OK ? "ok" :
          ((act == DV_ACT_PREV_CHAPTER || act == DV_ACT_NEXT_CHAPTER) &&
           !dvdnav_is_domain_vts(nav->dvdnav)) ? "ignored (not in a title)" :
          act == DV_ACT_ACTIVATE ? "no button, no still" :
          dvdnav_err_to_string(nav->dvdnav));
   if (st == DVDNAV_STATUS_OK)
      user_jump(ctx);
}

void dv_nav_pointer(dv_ctx_t *ctx, int x, int y, bool activate)
{
   dv_nav_t *nav = &ctx->nav;
   pci_t *pci = (pci_t *)dv_nav_presented_pci(ctx);

   if (!nav->dvdnav || !dv_nav_has_buttons(ctx))
      return;
   if (activate)
   {
      if (dvdnav_mouse_activate(nav->dvdnav, pci, x, y) == DVDNAV_STATUS_OK)
      {
         dv_log(ctx, RETRO_LOG_DEBUG, "pointer activate at %d,%d\n", x, y);
         user_jump(ctx);
      }
   }
   else
      dvdnav_mouse_select(nav->dvdnav, pci, x, y);
}
