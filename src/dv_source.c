/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Content resolution + libretro-VFS-backed stream callbacks (CLAUDE.md §5.1).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "dv_ctx.h"
#include "dv_source.h"

static bool has_ext(const char *path, const char *ext)
{
   const char *dot = strrchr(path, '.');
   return dot && strcasecmp(dot + 1, ext) == 0;
}

static bool is_dir(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void strip_last_component(char *path)
{
   char *sep = strrchr(path, '/');
#ifdef _WIN32
   char *bsep = strrchr(path, '\\');
   if (bsep > sep)
      sep = bsep;
#endif
   if (sep)
      *sep = '\0';
}

bool dv_source_resolve(dv_ctx_t *ctx, const char *content_path, dv_source_t *src)
{
   memset(src, 0, sizeof(*src));

   if (!content_path || !*content_path)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "No content path given.\n");
      return false;
   }
   if (strlen(content_path) >= sizeof(src->path))
   {
      dv_log(ctx, RETRO_LOG_ERROR, "Content path too long.\n");
      return false;
   }

   strcpy(src->path, content_path);

   if (has_ext(content_path, "iso") || has_ext(content_path, "img"))
   {
      src->kind = DV_SOURCE_IMAGE;
   }
   else if (has_ext(content_path, "ifo"))
   {
      /* .../VIDEO_TS/VIDEO_TS.IFO -> .../VIDEO_TS (then -> root below). */
      strip_last_component(src->path);
      src->kind = DV_SOURCE_DIRECTORY;
   }
   else if (is_dir(content_path))
   {
      src->kind = DV_SOURCE_DIRECTORY;
   }
   else
   {
      dv_log(ctx, RETRO_LOG_ERROR, "Unsupported content: %s\n", content_path);
      return false;
   }

   if (src->kind == DV_SOURCE_DIRECTORY)
   {
      /* Hand libdvdread the disc root (parent of VIDEO_TS): it looks for
       * <root>/VIDEO_TS first, and maps a mount root to its device for CSS. */
      const char *base = strrchr(src->path, '/');
      base = base ? base + 1 : src->path;
      if (strcasecmp(base, "VIDEO_TS") == 0)
         strip_last_component(src->path);
      if (!*src->path)
         strcpy(src->path, "/");
   }

   dv_log(ctx, RETRO_LOG_INFO, "Source: %s (%s)\n", src->path,
          src->kind == DV_SOURCE_IMAGE ? "image" : "directory");
   return true;
}

/* ---- stream callbacks ------------------------------------------------- */

/* libdvdread contract (dvd_input.c): pf_seek returns 0 on success, -1 on
 * error; pf_read returns bytes read (<0 on error). */

static int stream_seek(void *priv, uint64_t pos)
{
   dv_stream_t *st = priv;
   if (st->vfs_fh)
      return st->ctx->vfs->seek(st->vfs_fh, (int64_t)pos,
                                RETRO_VFS_SEEK_POSITION_START) < 0 ? -1 : 0;
#ifdef _WIN32
   return _fseeki64(st->fp, (__int64)pos, SEEK_SET) == 0 ? 0 : -1;
#else
   return fseeko(st->fp, (off_t)pos, SEEK_SET) == 0 ? 0 : -1;
#endif
}

static int stream_read(void *priv, void *buf, int len)
{
   dv_stream_t *st = priv;
   if (len <= 0)
      return 0;
   if (st->vfs_fh)
      return (int)st->ctx->vfs->read(st->vfs_fh, buf, (uint64_t)len);
   {
      size_t n = fread(buf, 1, (size_t)len, st->fp);
      if (n == 0 && ferror(st->fp))
         return -1;
      return (int)n;
   }
}

static dvd_reader_stream_cb stream_cb = {
   stream_seek,
   stream_read,
   NULL  /* pf_readv: optional, libdvdread falls back to pf_read */
};

dvd_reader_stream_cb *dv_stream_callbacks(void)
{
   return &stream_cb;
}

bool dv_stream_open(dv_ctx_t *ctx, const dv_source_t *src, dv_stream_t *st)
{
   memset(st, 0, sizeof(*st));
   st->ctx = ctx;

   if (ctx->vfs)
   {
      st->vfs_fh = ctx->vfs->open(src->path, RETRO_VFS_FILE_ACCESS_READ,
                                  RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (st->vfs_fh)
         return true;
      dv_log(ctx, RETRO_LOG_WARN, "VFS open failed, falling back to stdio: %s\n",
             src->path);
   }

   st->fp = fopen(src->path, "rb");
   if (!st->fp)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "Cannot open %s: %s\n", src->path, strerror(errno));
      return false;
   }
   return true;
}

void dv_stream_close(dv_stream_t *st)
{
   if (st->vfs_fh)
      st->ctx->vfs->close(st->vfs_fh);
   if (st->fp)
      fclose(st->fp);
   st->vfs_fh = NULL;
   st->fp     = NULL;
}

/* ---- CSS environment -------------------------------------------------- */

static void make_dir(const char *path)
{
#ifdef _WIN32
   _mkdir(path);
#else
   mkdir(path, 0755);
#endif
}

void dv_source_setup_css_env(dv_ctx_t *ctx)
{
   char path[4096 + 32];

   if (!*ctx->system_dir)
      return;

   /* Don't override a cache location the user set explicitly. */
   if (getenv("DVDCSS_CACHE"))
      return;

   snprintf(path, sizeof(path), "%s/deevee", ctx->system_dir);
   make_dir(path);
   snprintf(path, sizeof(path), "%s/deevee/dvdcss", ctx->system_dir);
   make_dir(path);

#ifdef _WIN32
   _putenv_s("DVDCSS_CACHE", path);
#else
   setenv("DVDCSS_CACHE", path, 1);
#endif
   dv_log(ctx, RETRO_LOG_INFO, "DVDCSS_CACHE=%s\n", path);
}
