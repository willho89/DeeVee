/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * libretro entry points (CLAUDE.md §3). The only file allowed globals.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <libretro.h>
#include <libavcodec/avcodec.h>

#include "dv_ctx.h"

#define DV_SAMPLE_RATE  48000
#define DV_FPS_NTSC     (60000.0 / 1001.0)
#define DV_FPS_PAL      50.0

/* Max packs read per retro_run. Normal playback needs ~10 (≈10 Mbit/s);
 * the cap only bounds a pathological stretch with no decodable video. */
#define DV_BLOCKS_PER_RUN 2048
/* At load: read up to this many packs looking for the first picture, so
 * initial geometry/timing (NTSC vs PAL) is right without a reinit. */
#define DV_PRIME_BLOCKS   16384

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_log_printf_t         log_cb;

static dv_ctx_t *g_ctx;
static bool      g_can_dupe;

/* What the frontend currently believes. */
static struct retro_system_av_info g_av;

/* One tick of audio: 48000/50 = 960 frames max; headroom for odd rates. */
#define DV_AUDIO_MAX_TICK 2048
static int16_t  g_audio_buf[2 * DV_AUDIO_MAX_TICK];
/* Video frame with the subpicture blended on (render.buf stays clean). */
static uint32_t g_composite[DV_MAX_WIDTH * DV_MAX_HEIGHT];
static const uint32_t *g_last_frame;   /* what video_cb was last given */
static dv_disp_t g_last_mode;
static double   g_audio_accum;

/* ---- logging ---------------------------------------------------------- */

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void dv_log(dv_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...)
{
   char buf[2048];
   va_list va;
   retro_log_printf_t cb = (ctx && ctx->log_cb) ? ctx->log_cb : (log_cb ? log_cb : fallback_log);

   va_start(va, fmt);
   vsnprintf(buf, sizeof(buf), fmt, va);
   va_end(va);
   cb(level, "[DeeVee] %s", buf);
}

static void show_message(dv_ctx_t *ctx, unsigned ms, bool alert, const char *fmt, va_list va)
{
   char buf[256];
   struct retro_message_ext msg;
   unsigned version = 0;

   vsnprintf(buf, sizeof(buf), fmt, va);
   dv_log(ctx, RETRO_LOG_DEBUG, "OSD: %s\n", buf);
   if (!ctx || (!ctx->opt.osd && !alert) || !environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) && version >= 1)
   {
      memset(&msg, 0, sizeof(msg));
      msg.msg      = buf;
      msg.duration = ms;
      msg.priority = alert ? 3 : 1;
      msg.level    = alert ? RETRO_LOG_WARN : RETRO_LOG_INFO;
      msg.target   = RETRO_MESSAGE_TARGET_OSD;
      msg.type     = alert ? RETRO_MESSAGE_TYPE_NOTIFICATION : RETRO_MESSAGE_TYPE_NOTIFICATION_ALT;
      msg.progress = -1;
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
   }
   else
   {
      struct retro_message old = { buf, (unsigned)(ms * 60 / 1000) };
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &old);
   }
}

void dv_osd(dv_ctx_t *ctx, unsigned ms, const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   show_message(ctx, ms, false, fmt, va);
   va_end(va);
}

void dv_osd_alert(dv_ctx_t *ctx, unsigned ms, const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   show_message(ctx, ms, true, fmt, va);
   va_end(va);
}

/* ---- libretro API ----------------------------------------------------- */

RETRO_API unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

/* ---- core options (CLAUDE.md §5.10; completed in M6) ------------------- */

static struct retro_core_option_v2_category g_opt_categories[] = {
   { "video", "Video", "Deinterlacing and aspect ratio." },
   { "audio", "Audio", "Audio track, downmix and dynamic range." },
   { "disc", "Disc & Region", "Region, menu language, copy protection and end of disc." },
   { "subtitles", "Subtitles", "Subtitle display and language." },
   { NULL, NULL, NULL },
};

#define DV_LANG_VALUES \
   { "auto", "Disc default" }, { "en", "English" }, { "fr", "French" }, \
   { "es", "Spanish" }, { "de", "German" }, { "it", "Italian" }, \
   { "pt", "Portuguese" }, { "nl", "Dutch" }, { "sv", "Swedish" }, \
   { "da", "Danish" }, { "no", "Norwegian" }, { "fi", "Finnish" }, \
   { "pl", "Polish" }, { "cs", "Czech" }, { "hu", "Hungarian" }, \
   { "el", "Greek" }, { "tr", "Turkish" }, { "ru", "Russian" }, \
   { "he", "Hebrew" }, { "ar", "Arabic" }, { "hi", "Hindi" }, \
   { "th", "Thai" }, { "ja", "Japanese" }, { "ko", "Korean" }, \
   { "zh", "Chinese" }, { NULL, NULL }

static struct retro_core_option_v2_definition g_opt_defs[] = {
   {
      "deevee_deinterlace", "Deinterlace", NULL,
      "Applied only to pictures flagged as interlaced; film is untouched. Field-rate modes "
      "show every field (smooth motion on video-shot content).",
      NULL, "video",
      { { "bwdif2x", "Bwdif, field rate" }, { "yadif2x", "Yadif, field rate" },
        { "yadif", "Yadif, frame rate" }, { "off", "Off" }, { NULL, NULL } },
      "bwdif2x"
   },
   {
      "deevee_aspect", "Aspect Ratio", NULL,
      "'Auto' uses the aspect signalled by the disc.",
      NULL, "video",
      { { "auto", "Auto" }, { "4:3", "4:3" }, { "16:9", "16:9" }, { NULL, NULL } },
      "auto"
   },
   {
      "deevee_tv_display", "TV Display Type", NULL,
      "The TV the virtual player drives, as in a hardware player's setup menu. On a 4:3 TV, "
      "widescreen video is letterboxed (black bars) or panned & scanned (sides cropped), and "
      "discs with separate 4:3 menus or subtitles use them. The disc's own menus may only "
      "pick up a change after reloading content.",
      NULL, "video",
      { { "16:9", "16:9 Widescreen" }, { "4:3_letterbox", "4:3 Letterbox" },
        { "4:3_panscan", "4:3 Pan & Scan" }, { NULL, NULL } },
      "16:9"
   },
   {
      "deevee_downmix", "Downmix", NULL,
      "How surround audio is folded to two channels. Pro Logic II keeps surround cues "
      "for a matrix decoder (AV receiver).",
      NULL, "audio",
      { { "stereo", "Stereo" }, { "dplii", "Dolby Pro Logic II" }, { NULL, NULL } },
      "stereo"
   },
   {
      "deevee_drc", "Dynamic Range Compression", NULL,
      "Dolby Digital loudness compression. 'Night' keeps dialogue audible at low volume.",
      NULL, "audio",
      { { "normal", "Normal" }, { "off", "Off (full range)" }, { "night", "Night (heavy)" },
        { NULL, NULL } },
      "normal"
   },
   {
      "deevee_end_of_disc", "End of Disc", NULL,
      "What to do when the disc's program ends.",
      NULL, "disc",
      { { "restart", "Restart disc" }, { "stop", "Stop" }, { "close", "Close content" },
        { NULL, NULL } },
      "restart"
   },
   {
      "deevee_osd", "Show OSD", NULL,
      "On-screen messages for chapters, seeking, track changes and pause.",
      NULL, "disc",
      { { "on", "On" }, { "off", "Off" }, { NULL, NULL } },
      "on"
   },
   {
      "deevee_region", "Player Region", NULL,
      "Region the virtual DVD player reports to the disc. 'Auto' picks a region the disc "
      "allows, which also passes region checks (RCE) in the disc's own menus. "
      "Applies when content is loaded.",
      NULL, "disc",
      { { "auto", "Auto (from disc)" }, { "1", "1 (US, Canada)" }, { "2", "2 (Europe, Japan)" },
        { "3", "3 (SE Asia)" }, { "4", "4 (Latin America, Oceania)" }, { "5", "5 (Africa, Russia)" },
        { "6", "6 (China)" }, { "7", "7" }, { "8", "8" }, { NULL, NULL } },
      "auto"
   },
   {
      "deevee_menu_language", "Menu Language", NULL,
      "Preferred language for disc menus, where the disc provides several. "
      "Applies when content is loaded.",
      NULL, "disc",
      { DV_LANG_VALUES },
      "auto"
   },
   {
      "deevee_css_method", "CSS Decryption Method", NULL,
      "How libdvdcss recovers keys for encrypted discs. 'Key' is fastest; try 'Disc' or "
      "'Title' if an encrypted disc fails. Keys are cached in the system directory. "
      "Applies when content is loaded.",
      NULL, "disc",
      { { "key", "Key (default)" }, { "disc", "Disc" }, { "title", "Title" }, { NULL, NULL } },
      "key"
   },
   {
      "deevee_audio_language", "Audio Language", NULL,
      "Preferred audio language, applied when a title starts.",
      NULL, "audio",
      { DV_LANG_VALUES },
      "auto"
   },
   {
      "deevee_subtitles", "Subtitles", NULL,
      "Subtitle display when a title starts. 'Disc default' follows the disc. "
      "Forced subtitles (e.g. translated signs) are always shown.",
      NULL, "subtitles",
      { { "auto", "Disc default" }, { "on", "On" }, { "off", "Off (forced only)" }, { NULL, NULL } },
      "auto"
   },
   {
      "deevee_subtitle_language", "Subtitle Language", NULL,
      "Preferred subtitle language, applied when a title starts.",
      NULL, "subtitles",
      { DV_LANG_VALUES },
      "auto"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 g_opts_v2 = { g_opt_categories, g_opt_defs };

static void set_core_options(retro_environment_t cb)
{
   unsigned version = 0;
   if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
   {
      cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &g_opts_v2);
      return;
   }
   {
      /* Legacy frontends: "Desc; default|other|...". */
#define DV_NUM_OPTS (sizeof(g_opt_defs) / sizeof(g_opt_defs[0]) - 1)
      static char desc[DV_NUM_OPTS][2048];
      static struct retro_variable vars[DV_NUM_OPTS + 1];
      unsigned i;
      for (i = 0; i < DV_NUM_OPTS; i++)
      {
         const struct retro_core_option_v2_definition *d = &g_opt_defs[i];
         size_t n = (size_t)snprintf(desc[i], sizeof(desc[i]), "%s; %s", d->desc, d->default_value);
         int v;
         for (v = 0; d->values[v].value && n < sizeof(desc[i]); v++)
            if (strcmp(d->values[v].value, d->default_value))
               n += (size_t)snprintf(desc[i] + n, sizeof(desc[i]) - n, "|%s", d->values[v].value);
         vars[i].key   = d->key;
         vars[i].value = desc[i];
      }
      vars[DV_NUM_OPTS].key = vars[DV_NUM_OPTS].value = NULL;
      cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
   }
}

static const char *get_option(const char *key)
{
   struct retro_variable var = { key, NULL };
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return var.value;
   return NULL;
}

static bool read_options(dv_ctx_t *ctx);

/* Push option values into the modules that cache them. */
static void apply_options(dv_ctx_t *ctx, const dv_options_t *old)
{
   dv_video_set_deint(ctx, (dv_deint_t)ctx->opt.deint);
   if (!old || old->dplii != ctx->opt.dplii || old->drc != ctx->opt.drc)
      dv_audio_reconfigure(ctx);   /* reopen decoder with new downmix/DRC */
   if (old && (old->audio_lang[0] != ctx->opt.audio_lang[0] ||
               old->audio_lang[1] != ctx->opt.audio_lang[1] ||
               old->sub_lang[0] != ctx->opt.sub_lang[0] ||
               old->sub_lang[1] != ctx->opt.sub_lang[1] ||
               old->subtitles != ctx->opt.subtitles))
      dv_nav_apply_title_options(ctx, true);
   if (old && (old->aspect != ctx->opt.aspect || old->tv_display != ctx->opt.tv_display))
      g_av.geometry.aspect_ratio = 0;   /* force a geometry update */
   if (old && old->tv_display != ctx->opt.tv_display)
      dv_nav_apply_display(ctx);
}

/* Returns true if anything changed. */
static bool read_options(dv_ctx_t *ctx)
{
   dv_options_t o;
   const char *v;

   memset(&o, 0, sizeof(o));
   v = get_option("deevee_subtitles");
   o.subtitles = (v && !strcmp(v, "on"))  ? DV_SUBS_ON :
                 (v && !strcmp(v, "off")) ? DV_SUBS_OFF : DV_SUBS_AUTO;
   v = get_option("deevee_subtitle_language");
   if (v && strlen(v) == 2)
      memcpy(o.sub_lang, v, 3);
   v = get_option("deevee_audio_language");
   if (v && strlen(v) == 2)
      memcpy(o.audio_lang, v, 3);
   v = get_option("deevee_menu_language");
   if (v && strlen(v) == 2)
      memcpy(o.menu_lang, v, 3);
   v = get_option("deevee_region");
   if (v && v[0] >= '1' && v[0] <= '8' && !v[1])
      o.region = v[0] - '0';
   v = get_option("deevee_deinterlace");
   o.deint = (v && !strcmp(v, "off"))     ? DV_DEINT_OFF :
             (v && !strcmp(v, "yadif"))   ? DV_DEINT_YADIF :
             (v && !strcmp(v, "yadif2x")) ? DV_DEINT_YADIF_2X : DV_DEINT_BWDIF_2X;
   v = get_option("deevee_aspect");
   o.aspect = (v && !strcmp(v, "4:3")) ? 1 : (v && !strcmp(v, "16:9")) ? 2 : 0;
   v = get_option("deevee_tv_display");
   o.tv_display = (v && !strcmp(v, "4:3_letterbox")) ? DV_TV_LETTERBOX :
                  (v && !strcmp(v, "4:3_panscan"))   ? DV_TV_PANSCAN : DV_TV_WIDE;
   v = get_option("deevee_downmix");
   o.dplii = v && !strcmp(v, "dplii");
   v = get_option("deevee_drc");
   o.drc = (v && !strcmp(v, "off")) ? 1 : (v && !strcmp(v, "night")) ? 2 : 0;
   v = get_option("deevee_end_of_disc");
   o.end_of_disc = (v && !strcmp(v, "stop")) ? 1 : (v && !strcmp(v, "close")) ? 2 : 0;
   v = get_option("deevee_osd");
   o.osd = !(v && !strcmp(v, "off"));
   v = get_option("deevee_css_method");
   if (v && (!strcmp(v, "disc") || !strcmp(v, "title")))
      snprintf(o.css_method, sizeof(o.css_method), "%s", v);

   if (!memcmp(&o, &ctx->opt, sizeof(o)))
      return false;
   ctx->opt = o;
   return true;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback logging;
   bool no_game = false;

   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;

   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
   set_core_options(cb);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "DeeVee";
   info->library_version  = DV_VERSION " (css:" DEEVEE_CSS_MODE ")";
   info->valid_extensions = "iso|ifo|img|chd";
   info->need_fullpath    = true;
   info->block_extract    = true;
}

static void default_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = 720;
   info->geometry.base_height  = 480;
   info->geometry.max_width    = DV_MAX_WIDTH;
   info->geometry.max_height   = DV_MAX_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = DV_FPS_NTSC;
   info->timing.sample_rate    = DV_SAMPLE_RATE;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   *info = g_av;
}

/* Output rate for the picture just decoded: 25 Hz content -> 50, else 59.94
 * (29.97 video and 23.976 film with pulldown). */
static double content_fps(const dv_ctx_t *ctx)
{
   AVRational fr = ctx->video.avctx->framerate;
   if (fr.den > 0 && fr.num == 25 * fr.den)
      return DV_FPS_PAL;
   return DV_FPS_NTSC;
}

/* Tell the frontend about geometry/timing changes of the rendered picture. */
static void update_av_info(dv_ctx_t *ctx)
{
   const dv_render_t *r = &ctx->render;
   double fps = content_fps(ctx);
   double dar = dv_render_output_dar(ctx);
   bool timing   = fps != g_av.timing.fps;
   bool geometry = r->width  != g_av.geometry.base_width ||
                   r->height != g_av.geometry.base_height ||
                   fabs(dar - g_av.geometry.aspect_ratio) > 0.001;

   if (!timing && !geometry)
      return;

   g_av.geometry.base_width   = r->width;
   g_av.geometry.base_height  = r->height;
   g_av.geometry.aspect_ratio = (float)dar;
   g_av.timing.fps            = fps;

   dv_log(ctx, RETRO_LOG_INFO, "Output: %ux%u, aspect %.4f, %.3f Hz\n",
          r->width, r->height, dar, fps);

   if (timing)
   {
      dv_clock_set_fps(&ctx->clock, fps);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &g_av);
   }
   else
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g_av.geometry);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

/* RetroArch "Restart": like powering a player off and on, start again from
 * the disc's First Play program. */
RETRO_API void retro_reset(void)
{
   if (!g_ctx)
      return;
   g_ctx->css_blocked = false;
   g_ctx->ps_stats.scrambled = 0;
   if (dv_nav_restart(g_ctx))
      dv_osd(g_ctx, 2000, "Restarting disc");
}

static void output_audio(dv_ctx_t *ctx)
{
   /* Fractional accumulation so 48000/59.94 = 800.8 averages out exactly. */
   size_t frames;
   g_audio_accum += DV_SAMPLE_RATE / g_av.timing.fps;
   frames = (size_t)g_audio_accum;
   g_audio_accum -= (double)frames;
   if (frames > DV_AUDIO_MAX_TICK)
      frames = DV_AUDIO_MAX_TICK;

   if (ctx->paused)
      memset(g_audio_buf, 0, frames * 2 * sizeof(int16_t));
   else
      dv_audio_render(ctx, g_audio_buf, frames);
   audio_batch_cb(g_audio_buf, frames);
}

RETRO_API void retro_run(void)
{
   dv_ctx_t *ctx = g_ctx;
   dv_render_t *r = &ctx->render;

   input_poll_cb();
   /* RetroArch sets (and may replace) the input callback after
    * retro_load_game, so never rely on one captured at load time. */
   ctx->input.state_cb = input_state_cb;
   dv_input_run(ctx);

   {
      bool updated = false;
      dv_options_t old = ctx->opt;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated &&
          read_options(ctx))
         apply_options(ctx, &old);
   }

   /* Fast-forward: skip non-reference (B) pictures so decoding keeps up. */
   {
      struct retro_throttle_state th;
      bool ff = environ_cb(RETRO_ENVIRONMENT_GET_THROTTLE_STATE, &th) &&
                th.mode == RETRO_THROTTLE_FAST_FORWARD;
      ctx->video.avctx->skip_frame = ff ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
   }

   if (!ctx->paused)
   {
      dv_clock_advance(&ctx->clock);
      dv_nav_fill(ctx, DV_BLOCKS_PER_RUN);
      dv_video_present(ctx);
      dv_nav_service(ctx);
   }
   dv_nav_update_highlight(ctx);

   /* End of disc (DVDNAV_STOP), once everything has been presented. */
   if (ctx->nav.stopped && !ctx->css_blocked &&
       dv_video_empty(&ctx->video) && dv_clock_drained(&ctx->clock))
   {
      if (ctx->opt.end_of_disc == 2)
         environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
      else if (ctx->opt.end_of_disc == 0)
      {
         dv_osd(ctx, 2000, "End of disc: restarting");
         dv_nav_restart(ctx);
      }
   }

   {
      const dv_subpic_t *sp = dv_spu_update(ctx);
      bool redraw = ctx->spu.changed;

      if (ctx->video.current_new)
      {
         ctx->video.current_new = false;
         if (dv_render_frame(ctx, ctx->video.current.frame))
         {
            update_av_info(ctx);
            redraw = true;
         }
      }
      ctx->spu.changed = false;

      if (dv_render_mode(ctx) != g_last_mode)
      {
         g_last_mode = dv_render_mode(ctx);
         redraw = true;
      }

      if (redraw && r->valid)
      {
         dv_disp_t mode = g_last_mode;
         const uint32_t *frame = r->buf;
         if (mode == DV_DISP_NORMAL || !dv_nav_overlay_native(ctx))
         {
            /* Overlay on the stored picture, then (4:3 TV) transform both. */
            if (sp)
            {
               dv_overlay_compose(ctx, r->buf, g_composite, r->width, r->height, sp,
                                  &ctx->highlight);
               frame = g_composite;
            }
            frame = dv_render_transform(ctx, frame, mode);
         }
         else
         {
            /* Letterbox/pan&scan SPU variant: authored for the 4:3 frame. */
            frame = dv_render_transform(ctx, r->buf, mode);
            if (sp)
            {
               dv_overlay_compose(ctx, frame, g_composite, r->width, r->height, sp,
                                  &ctx->highlight);
               frame = g_composite;
            }
         }
         g_last_frame = frame;
         video_cb(frame, r->width, r->height, r->width * sizeof(uint32_t));
      }
      else if (g_can_dupe && r->valid)
         video_cb(NULL, r->width, r->height, r->width * sizeof(uint32_t));
      else
         video_cb(g_last_frame ? g_last_frame : r->buf, r->width, r->height,
                  r->width * sizeof(uint32_t));
   }

   output_audio(ctx);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   struct retro_vfs_interface_info vfs_info = { 1, NULL };
   const char *sysdir = NULL;

   if (!game || !game->path)
      return false;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      dv_log(NULL, RETRO_LOG_ERROR, "XRGB8888 is not supported by the frontend.\n");
      return false;
   }

   g_ctx = calloc(1, sizeof(*g_ctx));
   if (!g_ctx)
      return false;
   g_ctx->environ_cb = environ_cb;
   g_ctx->log_cb     = log_cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info) && vfs_info.iface)
      g_ctx->vfs = vfs_info.iface;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) && sysdir)
      snprintf(g_ctx->system_dir, sizeof(g_ctx->system_dir), "%s", sysdir);

   dv_log(g_ctx, RETRO_LOG_INFO, "DeeVee %s (css:%s), VFS %s\n", DV_VERSION,
          DEEVEE_CSS_MODE, g_ctx->vfs ? "available" : "unavailable (stdio fallback)");

   g_can_dupe = false;
   environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe);
   {
      /* Resume points: fine for save/load, not for rewind, run-ahead or
       * netplay. */
      uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
      environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
   }
   dv_input_init(g_ctx, input_state_cb,
                 environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL));
   dv_input_set_descriptors(environ_cb);

   read_options(g_ctx);
   dv_source_load_css(g_ctx);
   dv_source_setup_css_env(g_ctx);
   dv_source_setup_css_method(g_ctx);
   dv_clock_init(&g_ctx->clock, DV_FPS_NTSC);

   dv_overlay_init(g_ctx);
   if (!dv_video_init(g_ctx) || !dv_render_init(g_ctx) || !dv_audio_init(g_ctx) ||
       !dv_spu_init(g_ctx) ||
       !dv_source_resolve(g_ctx, game->path, &g_ctx->source) ||
       !dv_nav_open(g_ctx, &g_ctx->source))
   {
      dv_video_free(g_ctx);
      dv_render_free(g_ctx);
      dv_audio_free(g_ctx);
      dv_spu_free(g_ctx);
      dv_source_unload_css(g_ctx);
      free(g_ctx);
      g_ctx = NULL;
      return false;
   }

   dv_nav_log_disc_info(g_ctx, &g_ctx->source);
   apply_options(g_ctx, NULL);

   /* Prime: decode up to the first picture so the first av_info is right. */
   dv_nav_fill(g_ctx, DV_PRIME_BLOCKS);
   default_av_info(&g_av);
   dv_video_decode_ahead(g_ctx);
   if (g_ctx->video.qcount)
   {
      const dv_vframe_t *first = &g_ctx->video.queue[g_ctx->video.qhead];
      if (dv_render_frame(g_ctx, first->frame))
      {
         g_av.geometry.base_width   = g_ctx->render.width;
         g_av.geometry.base_height  = g_ctx->render.height;
         g_av.geometry.aspect_ratio = (float)dv_render_output_dar(g_ctx);
         g_av.timing.fps            = content_fps(g_ctx);
      }
   }
   else
      dv_log(g_ctx, RETRO_LOG_WARN, "No picture found while priming.\n");
   dv_clock_set_fps(&g_ctx->clock, g_av.timing.fps);
   dv_log(g_ctx, RETRO_LOG_INFO, "Initial output: %ux%u, aspect %.4f, %.3f Hz\n",
          g_av.geometry.base_width, g_av.geometry.base_height,
          g_av.geometry.aspect_ratio, g_av.timing.fps);

   g_audio_accum = 0.0;
   g_last_frame  = NULL;
   g_last_mode   = dv_render_mode(g_ctx);
   return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}

RETRO_API void retro_unload_game(void)
{
   if (!g_ctx)
      return;
   {
      const dv_video_t *v = &g_ctx->video;
      const dv_nav_t *n = &g_ctx->nav;
      const dv_ps_stats_t *ps = &g_ctx->ps_stats;
      dv_log(g_ctx, RETRO_LOG_INFO,
             "Stats: packs %u (video PES %u, audio %u, spu %u, other %u, errors %u); "
             "stills %u, waits %u, hops %u, vobu discontinuities %u, scrambled %u\n",
             n->blocks, ps->video_pes, ps->audio_pes, ps->spu_pes, ps->other_pes,
             ps->errors, n->stills, n->waits, n->hops, n->discontinuities, ps->scrambled);
      dv_log(g_ctx, RETRO_LOG_INFO,
             "Video: decoded %u, presented %u, dropped %u, overflow %u, interpolated %u, errors %u; "
             "cadence (ticks/frame) 1:%u 2:%u 3:%u 4:%u 5+:%u\n",
             v->decoded, v->presented, v->dropped, v->overflow, v->interpolated, v->errors,
             v->cadence[1], v->cadence[2], v->cadence[3], v->cadence[4],
             v->cadence[5] + v->cadence[6] + v->cadence[7]);
   }
   {
      const dv_audio_t *a = &g_ctx->audio;
      dv_log(g_ctx, RETRO_LOG_INFO,
             "Audio: packets %u, blocks %u, decoder opens %u, errors %u, overflow %u, segments %u (unconfirmed PTS %u); "
             "out %llu, silence %llu, dropped %llu, input gaps %llu (frames)\n",
             a->packets, a->decoded_blocks, a->reopen, a->errors, a->overflow, a->segments, a->suspect,
             (unsigned long long)a->out_frames, (unsigned long long)a->silence_frames,
             (unsigned long long)a->dropped_frames, (unsigned long long)a->gap_frames);
   }
   dv_log(g_ctx, RETRO_LOG_INFO, "Subpictures: packets %u, decoded %u, displayed %u, errors %u, unsupported cmds %u\n",
          g_ctx->spu.packets, g_ctx->spu.decoded, g_ctx->spu.displayed, g_ctx->spu.errors,
          g_ctx->spu.unsupported);
   dv_nav_close(g_ctx);
   dv_video_free(g_ctx);
   dv_render_free(g_ctx);
   dv_audio_free(g_ctx);
   dv_spu_free(g_ctx);
   dv_source_unload_css(g_ctx);
   free(g_ctx);
   g_ctx = NULL;
}

RETRO_API unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

/* Save states are resume points (§5.12, D-031): where the disc is and what
 * its program has stored, not a frame-exact snapshot. */
RETRO_API size_t retro_serialize_size(void)
{
   return g_ctx ? dv_nav_state_size() : 0;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
   return g_ctx && dv_nav_save_state(g_ctx, (uint8_t *)data, size);
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
   return g_ctx && dv_nav_load_state(g_ctx, (const uint8_t *)data, size);
}
RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { }
RETRO_API void *retro_get_memory_data(unsigned id) { return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
