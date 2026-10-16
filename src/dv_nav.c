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
#include <math.h>
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
   /* Normal without libdvdcss (decrypted rips); dv_ps reports discs that
    * actually need it. */
   if (rl == RETRO_LOG_WARN && strstr(buf, "Encrypted DVD support unavailable"))
      rl = RETRO_LOG_INFO;
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

/* SPRM 14 (player video configuration) from the TV Display Type option, so
 * the disc's own programs see the TV a hardware player would report. Needs
 * the DeeVee libdvdnav patch (patches/libdvdnav). */
static void set_video_config(dv_ctx_t *ctx)
{
   static const int tv_aspect[] = { 3, 0, 0 };   /* 16:9 TV, 4:3 TV */
   static const int mode[]      = { 0, 2, 1 };   /* normal, letterbox, pan&scan */
   int tv = ctx->opt.tv_display;

   if (dvdnav_set_player_video_config(ctx->nav.dvdnav, tv_aspect[tv], mode[tv]) != DVDNAV_STATUS_OK)
      dv_log(ctx, RETRO_LOG_WARN, "dvdnav: set_player_video_config: %s\n",
             dvdnav_err_to_string(ctx->nav.dvdnav));
}

bool dv_nav_open(dv_ctx_t *ctx, const dv_source_t *src)
{
   dv_nav_t *nav = &ctx->nav;
   dvdnav_logger_cb logcb = { nav_log };
   dvdnav_status_t st;

   memset(nav, 0, sizeof(*nav));
   nav->stream.ctx    = ctx;
   nav->policy_title  = -1;
   nav->seek_target   = -1;

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

   /* Preference registers (SPRM 0/16/18); many discs' own programs read
    * these to pick menus and tracks. */
   dvdnav_menu_language_select(nav->dvdnav, ctx->opt.menu_lang[0] ? ctx->opt.menu_lang : "en");
   dvdnav_audio_language_select(nav->dvdnav, ctx->opt.audio_lang[0] ? ctx->opt.audio_lang : "en");
   dvdnav_spu_language_select(nav->dvdnav, ctx->opt.sub_lang[0] ? ctx->opt.sub_lang : "en");

   if (dvdnav_get_disk_region_mask(nav->dvdnav, &nav->disc_region_mask) != DVDNAV_STATUS_OK)
      nav->disc_region_mask = 0xFF;
   nav->vm_region = ctx->opt.region ? ctx->opt.region : pick_region(nav->disc_region_mask);
   dvdnav_set_region_mask(nav->dvdnav, 1 << (nav->vm_region - 1));
   set_video_config(ctx);
   if (ctx->opt.region && !(nav->disc_region_mask & (1 << (ctx->opt.region - 1))))
      dv_log(ctx, RETRO_LOG_WARN, "Region %d forced but the disc only allows mask 0x%02X: "
             "its menus may refuse to play\n", ctx->opt.region,
             (unsigned)nav->disc_region_mask & 0xFF);

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
   dv_log(ctx, RETRO_LOG_INFO, "Region mask    : 0x%02X (regions %s); VM region set to %d%s\n",
          (unsigned)nav->disc_region_mask & 0xFF, regions, nav->vm_region,
          ctx->opt.region ? " (forced)" : " (auto)");
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
      slot->ptime = dvdnav_get_current_time(nav->dvdnav);
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

   if (!nav->dvdnav || ctx->css_blocked)
      return;

   for (i = 0; i < max_blocks && !ctx->css_blocked; i++)
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
            nav->permitted_df = dvdnav_get_video_scale_permission(nav->dvdnav);
            dv_spu_clear(ctx);
            dv_log(ctx, RETRO_LOG_INFO, "VTS %d -> %d (domain %d), video %ux%u, aspect code %u, "
                   "permitted display %u\n", ev->old_vtsN, ev->new_vtsN, ev->new_domain, w, h,
                   nav->ifo_aspect, nav->permitted_df);
            break;
         }

         case DVDNAV_CELL_CHANGE:
         {
            dvdnav_cell_change_event_t *ev = (dvdnav_cell_change_event_t *)buf;
            int32_t title = 0, part = 0;
            nav->pgc_length = ev->pgc_length;
            /* Same fields VLC logs for DVDNAV_CELL_CHANGE (oracle diffs, M6c). */
            dv_log(ctx, RETRO_LOG_DEBUG, "navstate: cellN=%d pgN=%d pgc_length=%lld cell_start=%lld\n",
                   ev->cellN, ev->pgN, (long long)ev->pgc_length, (long long)ev->cell_start);
            if (dvdnav_current_title_info(nav->dvdnav, &title, &part) == DVDNAV_STATUS_OK)
            {
               dv_log(ctx, RETRO_LOG_DEBUG, "cell change: title %d chapter %d\n", title, part);
               /* Announce chapter changes within titles (read-ahead means
                * this shows a little early; good enough for an OSD). */
               if (title > 0 && (title != nav->last_title || part != nav->last_part))
               {
                  int32_t parts = 0;
                  dvdnav_get_number_of_parts(nav->dvdnav, title, &parts);
                  dv_osd(ctx, 2000, "Title %d · Chapter %d/%d", title, part, parts);
               }
               nav->last_title = title;
               nav->last_part  = part;
            }
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
            dv_nav_apply_title_options(ctx, false);
            dv_spu_select(ctx, dv_nav_active_spu(ctx));
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

   /* Report where a seek landed, once its first VOBU is on screen. */
   if (nav->seek_target >= 0 && ctx->clock.anchored && nav->pci_count)
   {
      int64_t at = dv_nav_presented_time(ctx);
      dv_log(ctx, RETRO_LOG_INFO, "seek landed at %.2f s (target %.2f s, error %+.2f s)\n",
             at / 90000.0, nav->seek_target / 90000.0, (at - nav->seek_target) / 90000.0);
      nav->seek_target = -1;
   }

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

/* Select the logical stream whose language matches (audio or subtitle). */
static void select_by_language(dv_ctx_t *ctx, const char *lang, bool audio, int32_t title)
{
   dv_nav_t *nav = &ctx->nav;
   uint16_t want = (uint16_t)((lang[0] << 8) | lang[1]);
   int logical, n = audio ? 8 : 32;

   for (logical = 0; logical < n; logical++)
   {
      uint16_t have = audio ? dvdnav_audio_stream_to_lang(nav->dvdnav, (uint8_t)logical)
                            : dvdnav_spu_stream_to_lang(nav->dvdnav, (uint8_t)logical);
      if (have != want)
         continue;
      if (dvdnav_set_active_stream(nav->dvdnav, (uint8_t)logical,
                                   audio ? DVD_AUDIO_STREAM : DVD_SUBTITLE_STREAM) == DVDNAV_STATUS_OK)
         dv_log(ctx, RETRO_LOG_INFO, "%s: title %d, '%s' = logical stream %d\n",
                audio ? "Audio" : "Subtitles", title, lang, logical);
      return;
   }
   dv_log(ctx, RETRO_LOG_INFO, "%s: title %d has no '%s' stream\n",
          audio ? "Audio" : "Subtitles", title, lang);
}

void dv_nav_apply_title_options(dv_ctx_t *ctx, bool force)
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

   if (o->audio_lang[0])
   {
      select_by_language(ctx, o->audio_lang, true, title);
      dv_audio_select(ctx, dvdnav_get_active_audio_stream(nav->dvdnav));
   }
   if (o->sub_lang[0])
      select_by_language(ctx, o->sub_lang, false, title);

   if (o->subtitles == DV_SUBS_ON)
      dvdnav_toggle_spu_stream(nav->dvdnav, 1);
   else if (o->subtitles == DV_SUBS_OFF)
      dvdnav_toggle_spu_stream(nav->dvdnav, 0);

   dv_spu_select(ctx, dv_nav_active_spu(ctx));
}

/* ---- display type (M6b, D-025) ---------------------------------------- */

int dv_nav_active_spu(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   double dar;
   dv_disp_t mode;
   int8_t wide, alt;

   nav->spu_native = false;
   if (!nav->dvdnav)
      return -1;
   wide = dvdnav_get_active_spu_stream(nav->dvdnav);

   /* The VM's current aspect (the stream being read), not the picture on
    * screen: this picks what gets decoded next. */
   dar = ctx->opt.aspect == 1 ? 4.0 / 3.0 :
         ctx->opt.aspect == 2 ? 16.0 / 9.0 :
         nav->ifo_aspect == 3 ? 16.0 / 9.0 : 4.0 / 3.0;
   mode = dv_render_mode_for(ctx, dar);
   if (mode == DV_DISP_NORMAL || wide == -1)
      return wide;

   alt = dvdnav_get_active_spu_stream_mode(nav->dvdnav, (int32_t)mode);
   if (alt == -1)
      return wide;
   /* Discs without a separate variant point every mode at the wide
    * stream; that one is drawn on the stored picture and transformed. */
   nav->spu_native = (alt & 0x1F) != (wide & 0x1F);
   return alt;
}

void dv_nav_apply_display(dv_ctx_t *ctx)
{
   if (!ctx->nav.dvdnav)
      return;
   set_video_config(ctx);
   dv_spu_select(ctx, dv_nav_active_spu(ctx));
   ctx->spu.changed = true;
}

bool dv_nav_overlay_native(dv_ctx_t *ctx)
{
   return ctx->nav.spu_native && dv_render_mode(ctx) != DV_DISP_NORMAL;
}

/* Button group for the picture on screen. PCIs may carry up to three groups
 * (36/18/12 buttons each), one per display type (nav_types.h: 0 = 4:3,
 * bit 0 wide, bit 1 letterbox, bit 2 pan&scan). libdvdnav only ever looks
 * at group 1; its navigation links are shared by all groups on the discs
 * seen so far, so only the highlight rectangle and hit-testing use this. */
static int button_group(dv_ctx_t *ctx, const pci_t *pci)
{
   const hl_gi_t *gi = &pci->hli.hl_gi;
   int types[3] = { gi->btngr1_dsp_ty, gi->btngr2_dsp_ty, gi->btngr3_dsp_ty };
   int n = gi->btngr_ns, want, g;

   if (n <= 1)
      return 0;
   if (n > 3)
      n = 3;
   if (dv_nav_overlay_native(ctx))
      want = dv_render_mode(ctx) == DV_DISP_LETTERBOX ? 2 : 4;
   else
      want = dv_render_source_dar(ctx) > 1.5 ? 1 : 0;
   for (g = 0; g < n; g++)
      if (want ? (types[g] & want) != 0 : types[g] == 0)
         return g;
   return 0;
}

static const btni_t *group_button(dv_ctx_t *ctx, const pci_t *pci, int button)
{
   int n = pci->hli.hl_gi.btngr_ns;
   int per = n == 2 ? 18 : n == 3 ? 12 : 36;

   if (button <= 0 || button > pci->hli.hl_gi.btn_ns || button > per)
      return NULL;
   return &pci->hli.btnit[button_group(ctx, pci) * per + button - 1];
}

/* ---- menus and input (CLAUDE.md §5.5 highlight, §5.9 input) ------------ */

const pci_t *dv_nav_presented_pci(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   const dv_clock_t *c = &ctx->clock;
   double now = c->now + c->tick * 0.5;

   if (!nav->pci_count)
      return NULL;
   /* With the clock held (WAIT, still) nothing re-anchors it: use the held
    * time, and in a still the newest PCI (reading has stopped, it's the one
    * the still belongs to). Otherwise a menu reached when a title ends
    * kept the title's button-less PCIs on screen and showed no highlight
    * (TIMERIC, M6c; same cause as the held-clock SPU case in dv_spu). */
   if (!c->anchored)
      now = nav->still ? INFINITY : c->now;
   /* Drop PCIs superseded by a later one that is already on screen. */
   while (nav->pci_count >= 2 &&
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
      const btni_t *b;
      pci = dv_nav_presented_pci(ctx);
      b = group_button(ctx, pci, button);
      if (b && dvdnav_get_highlight_area((pci_t *)pci, button, 0, &area) == DVDNAV_STATUS_OK)
      {
         int i;
         hl.active = true;
         hl.sx = (int)b->x_start; hl.ex = (int)b->x_end;
         hl.sy = (int)b->y_start; hl.ey = (int)b->y_end;
         if (b->btn_coln)
            area.palette = pci->hli.btn_colit.btn_coli[b->btn_coln - 1][0];
         for (i = 0; i < 4; i++)
         {
            hl.color[i] = (uint8_t)((area.palette >> (16 + 4 * i)) & 0x0F);
            hl.alpha[i] = (uint8_t)((area.palette >> (4 * i)) & 0x0F);
         }
      }
   }

   /* Button link table of each new menu PCI (oracle walks, M6c). */
   pci = dv_nav_presented_pci(ctx);
   if (pci && pci->hli.hl_gi.hli_ss && pci->hli.hl_gi.btn_ns &&
       pci->pci_gi.nv_pck_lbn != nav->logged_btn_lbn &&
       memcmp(&pci->hli.btnit, &nav->logged_btnit, sizeof(nav->logged_btnit)))
   {
      char line[1024];
      int b, n = 0, cnt = pci->hli.hl_gi.btn_ns;
      for (b = 0; b < cnt && b < 36 && n < (int)sizeof(line) - 32; b++)
      {
         const btni_t *bt = &pci->hli.btnit[b];
         n += snprintf(line + n, sizeof(line) - n, " %d:%d/%d/%d/%d%s", b + 1, bt->up, bt->down,
                       bt->left, bt->right, bt->auto_action_mode ? "a" : "");
      }
      dv_log(ctx, RETRO_LOG_DEBUG, "buttons (u/d/l/r):%s\n", line);
      memcpy(&nav->logged_btnit, &pci->hli.btnit, sizeof(nav->logged_btnit));
   }
   if (pci)
      nav->logged_btn_lbn = pci->pci_gi.nv_pck_lbn;

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
   ctx->paused = false;
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

static void cycle_audio(dv_ctx_t *ctx);
static void cycle_subtitles(dv_ctx_t *ctx);
static void cycle_angle(dv_ctx_t *ctx);
static void osd_info(dv_ctx_t *ctx);

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
         else if (dvdnav_is_domain_vts(nav->dvdnav))
         {
            /* §5.9: A = play/pause during a title. */
            ctx->paused = !ctx->paused;
            dv_osd(ctx, ctx->paused ? 60000 : 1000, ctx->paused ? "❚❚ Paused" : "▶ Play");
            return;
         }
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
      /* No VM jump for these: handled and done. */
      case DV_ACT_CYCLE_AUDIO: cycle_audio(ctx);     return;
      case DV_ACT_CYCLE_SUBS:  cycle_subtitles(ctx); return;
      case DV_ACT_CYCLE_ANGLE: cycle_angle(ctx);     return;
      case DV_ACT_INFO:        osd_info(ctx);        return;
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

bool dv_nav_restart(dv_ctx_t *ctx)
{
   user_jump(ctx);
   dv_nav_close(ctx);
   dv_spu_select(ctx, -1);
   dv_audio_select(ctx, -1);
   return dv_nav_open(ctx, &ctx->source);
}

int64_t dv_nav_presented_time(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   const dv_pci_slot_t *slot;
   double into;

   if (!dv_nav_presented_pci(ctx))
      return -1;
   slot = &nav->pcis[nav->pci_head];
   into = ctx->clock.anchored ? ctx->clock.now - (double)slot->start : 0.0;
   if (into < 0)
      into = 0;
   return slot->ptime + (int64_t)into;
}

bool dv_nav_in_title(dv_ctx_t *ctx)
{
   return ctx->nav.dvdnav && dvdnav_is_domain_vts(ctx->nav.dvdnav) && !dv_nav_has_buttons(ctx);
}

static void format_time(int64_t t, char *out, size_t len)
{
   int64_t s = t < 0 ? 0 : t / 90000;
   snprintf(out, len, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

void dv_nav_seek(dv_ctx_t *ctx, int64_t delta)
{
   dv_nav_t *nav = &ctx->nav;
   int64_t cur = dv_nav_presented_time(ctx), target;
   char a[16], b[16];

   if (!nav->dvdnav || !dvdnav_is_domain_vts(nav->dvdnav) || cur < 0)
      return;
   target = cur + delta;
   if (target < 0)
      target = 0;
   if (nav->pgc_length > 90000 && target > nav->pgc_length - 90000)
      target = nav->pgc_length - 90000;   /* stay inside the PGC */

   if (dvdnav_jump_to_sector_by_time(nav->dvdnav, (uint64_t)target, 0) != DVDNAV_STATUS_OK)
   {
      dv_log(ctx, RETRO_LOG_WARN, "seek to %lld failed: %s\n", (long long)target,
             dvdnav_err_to_string(nav->dvdnav));
      return;
   }
   nav->seek_target = target;
   dv_log(ctx, RETRO_LOG_DEBUG, "seek: %lld -> %lld\n", (long long)cur, (long long)target);
   user_jump(ctx);
   format_time(target, a, sizeof(a));
   format_time(nav->pgc_length, b, sizeof(b));
   dv_osd(ctx, 1500, "%s %s / %s", delta < 0 ? "◀◀" : "▶▶", a, b);
}

static void osd_audio(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   int8_t phys = dvdnav_get_active_audio_stream(nav->dvdnav);
   int8_t logical = phys >= 0 ? dvdnav_get_audio_logical_stream(nav->dvdnav, (uint8_t)phys) : -1;
   uint16_t lang, fmt, ch;
   const char *fname;

   if (logical < 0)
   {
      dv_osd(ctx, 2000, "Audio: none");
      return;
   }
   lang = dvdnav_audio_stream_to_lang(nav->dvdnav, (uint8_t)logical);
   fmt  = dvdnav_audio_stream_format(nav->dvdnav, (uint8_t)logical);
   ch   = dvdnav_audio_stream_channels(nav->dvdnav, (uint8_t)logical);
   fname = fmt == DVD_AUDIO_FORMAT_AC3 ? "Dolby Digital" :
           fmt == DVD_AUDIO_FORMAT_DTS ? "DTS" :
           fmt == DVD_AUDIO_FORMAT_LPCM ? "LPCM" :
           (fmt == DVD_AUDIO_FORMAT_MPEG2_EXT || fmt == DVD_AUDIO_FORMAT_MPEG) ? "MPEG" : "?";
   if (lang != 0xFFFF && lang)
      dv_osd(ctx, 2000, "Audio %d: %c%c, %s %u ch", logical + 1, lang >> 8, lang & 0xFF, fname, ch);
   else
      dv_osd(ctx, 2000, "Audio %d: %s %u ch", logical + 1, fname, ch);
}

/* Next logical stream the PGC allows, after 'from' (wrapping). */
static int next_stream(dv_ctx_t *ctx, int from, int count, dvdnav_stream_type_t type, bool wrap)
{
   int i;
   for (i = 1; i <= count; i++)
   {
      int s = from + i;
      if (!wrap && s >= count)
         return -1;
      s %= count;
      if (dvdnav_set_active_stream(ctx->nav.dvdnav, (uint8_t)s, type) == DVDNAV_STATUS_OK)
         return s;
   }
   return -1;
}

static void cycle_audio(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   int8_t phys = dvdnav_get_active_audio_stream(nav->dvdnav);
   int cur = phys >= 0 ? dvdnav_get_audio_logical_stream(nav->dvdnav, (uint8_t)phys) : -1;
   if (next_stream(ctx, cur, 8, DVD_AUDIO_STREAM, true) >= 0)
      dv_audio_select(ctx, dvdnav_get_active_audio_stream(nav->dvdnav));
   osd_audio(ctx);
}

/* Off -> first stream -> ... -> last stream -> off. */
static void cycle_subtitles(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   int8_t active = dvdnav_get_active_spu_stream(nav->dvdnav);
   unsigned u = (unsigned)active & 0xFF;
   bool shown = active != -1 && !(u & 0x80);
   int logical = active == -1 ? -1 : dvdnav_get_spu_logical_stream(nav->dvdnav, (uint8_t)(u & 0x1F));
   int next;

   if (!shown)
   {
      next = next_stream(ctx, -1, 32, DVD_SUBTITLE_STREAM, false);
      if (next >= 0)
         dvdnav_toggle_spu_stream(nav->dvdnav, 1);
   }
   else
   {
      next = next_stream(ctx, logical, 32, DVD_SUBTITLE_STREAM, false);
      if (next < 0)
         dvdnav_toggle_spu_stream(nav->dvdnav, 0);
   }
   dv_spu_select(ctx, dv_nav_active_spu(ctx));

   if (next < 0)
      dv_osd(ctx, 2000, "Subtitles: off");
   else
   {
      uint16_t lang = dvdnav_spu_stream_to_lang(nav->dvdnav, (uint8_t)next);
      if (lang != 0xFFFF && lang)
         dv_osd(ctx, 2000, "Subtitles %d: %c%c", next + 1, lang >> 8, lang & 0xFF);
      else
         dv_osd(ctx, 2000, "Subtitles %d", next + 1);
   }
}

static void cycle_angle(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   int32_t cur = 0, num = 0;
   if (dvdnav_get_angle_info(nav->dvdnav, &cur, &num) != DVDNAV_STATUS_OK || num <= 1)
   {
      dv_osd(ctx, 1500, "Only one angle");
      return;
   }
   /* Seamless: libdvdnav switches at the next interleaved unit. */
   dvdnav_angle_change(nav->dvdnav, cur % num + 1);
   dv_osd(ctx, 1500, "Angle %d/%d", cur % num + 1, num);
}

static void osd_info(dv_ctx_t *ctx)
{
   dv_nav_t *nav = &ctx->nav;
   int32_t title = 0, part = 0, parts = 0;
   char a[16], b[16];
   dvdnav_current_title_info(nav->dvdnav, &title, &part);
   dvdnav_get_number_of_parts(nav->dvdnav, title, &parts);
   format_time(dv_nav_presented_time(ctx), a, sizeof(a));
   format_time(nav->pgc_length, b, sizeof(b));
   dv_osd(ctx, 3000, "Title %d · Chapter %d/%d · %s / %s", title, part, parts, a, b);
}

void dv_nav_pointer(dv_ctx_t *ctx, int x, int y, bool activate)
{
   dv_nav_t *nav = &ctx->nav;
   pci_t *pci = (pci_t *)dv_nav_presented_pci(ctx);

   dv_disp_t mode = dv_render_mode(ctx);
   int b, best = 0, dist = 0x08000000;

   if (!nav->dvdnav || !dv_nav_has_buttons(ctx))
      return;
   /* x, y are in the 4:3 output frame; buttons are authored either for it
    * (letterbox/pan&scan variant) or for the stored picture. */
   if (mode != DV_DISP_NORMAL && !dv_nav_overlay_native(ctx))
      dv_render_unmap(ctx, mode, &x, &y);

   /* As libdvdnav dvdnav_mouse_select(), but over the button group on
    * screen: the button containing the point whose centre is nearest. */
   for (b = 1; b <= pci->hli.hl_gi.btn_ns; b++)
   {
      const btni_t *bt = group_button(ctx, pci, b);
      int dx, dy, d;
      if (!bt || x < (int)bt->x_start || x > (int)bt->x_end ||
          y < (int)bt->y_start || y > (int)bt->y_end)
         continue;
      dx = ((int)bt->x_start + (int)bt->x_end) / 2 - x;
      dy = ((int)bt->y_start + (int)bt->y_end) / 2 - y;
      d  = dx * dx + dy * dy;
      if (d < dist)
      {
         dist = d;
         best = b;
      }
   }
   if (!best)
      return;

   if (activate)
   {
      if (dvdnav_button_select_and_activate(nav->dvdnav, pci, best) == DVDNAV_STATUS_OK)
      {
         dv_log(ctx, RETRO_LOG_DEBUG, "pointer activate button %d at %d,%d\n", best, x, y);
         user_jump(ctx);
      }
   }
   else
   {
      int32_t cur = 0;
      dvdnav_get_current_highlight(nav->dvdnav, &cur);
      if (cur != best)
         dvdnav_button_select(nav->dvdnav, pci, best);
   }
}
