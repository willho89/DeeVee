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
   else if (has_ext(content_path, "chd"))
   {
      src->kind   = DV_SOURCE_IMAGE;
      src->is_chd = true;
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
          src->kind == DV_SOURCE_DIRECTORY ? "directory" : src->is_chd ? "CHD image" : "image");
   return true;
}

/* ---- raw file I/O (libretro VFS, stdio fallback) ----------------------- */

static int raw_seek(dv_stream_t *st, int64_t pos, int whence)
{
   if (st->vfs_fh)
   {
      int w = whence == SEEK_END ? RETRO_VFS_SEEK_POSITION_END :
              whence == SEEK_CUR ? RETRO_VFS_SEEK_POSITION_CURRENT :
                                   RETRO_VFS_SEEK_POSITION_START;
      return st->ctx->vfs->seek(st->vfs_fh, pos, w) < 0 ? -1 : 0;
   }
#ifdef _WIN32
   return _fseeki64(st->fp, (__int64)pos, whence) == 0 ? 0 : -1;
#else
   return fseeko(st->fp, (off_t)pos, whence) == 0 ? 0 : -1;
#endif
}

static int64_t raw_read(dv_stream_t *st, void *buf, uint64_t len)
{
   if (st->vfs_fh)
      return st->ctx->vfs->read(st->vfs_fh, buf, len);
   {
      size_t n = fread(buf, 1, (size_t)len, st->fp);
      if (n == 0 && ferror(st->fp))
         return -1;
      return (int64_t)n;
   }
}

static int64_t raw_size(dv_stream_t *st)
{
   if (st->vfs_fh)
      return st->ctx->vfs->size(st->vfs_fh);
#ifdef _WIN32
   {
      __int64 cur = _ftelli64(st->fp), end;
      _fseeki64(st->fp, 0, SEEK_END);
      end = _ftelli64(st->fp);
      _fseeki64(st->fp, cur, SEEK_SET);
      return end;
   }
#else
   {
      off_t cur = ftello(st->fp), end;
      fseeko(st->fp, 0, SEEK_END);
      end = ftello(st->fp);
      fseeko(st->fp, cur, SEEK_SET);
      return end;
   }
#endif
}

/* ---- CHD (libchdr) ----------------------------------------------------- */

/* libchdr reads the .chd container through these. Its fclose is a no-op:
 * the raw handle is ours and closed in dv_stream_close(). */
static uint64_t chd_cb_fsize(void *p)
{
   int64_t n = raw_size(p);
   return n < 0 ? (uint64_t)-1 : (uint64_t)n;
}

static size_t chd_cb_fread(void *buf, size_t size, size_t count, void *p)
{
   int64_t n;
   if (!size || !count)
      return 0;
   n = raw_read(p, buf, (uint64_t)size * count);
   return n <= 0 ? 0 : (size_t)n / size;
}

static int chd_cb_fclose(void *p)
{
   (void)p;
   return 0;
}

static int chd_cb_fseek(void *p, int64_t off, int whence)
{
   return raw_seek(p, off, whence);
}

static const core_file_callbacks chd_callbacks = {
   chd_cb_fsize, chd_cb_fread, chd_cb_fclose, chd_cb_fseek
};

static bool chd_attach(dv_ctx_t *ctx, dv_stream_t *st, const char *path)
{
   const chd_header *hdr;
   char meta[256];
   uint32_t metalen = 0;
   chd_error err = chd_open_core_file_callbacks(&chd_callbacks, st, CHD_OPEN_READ, NULL, &st->chd);

   if (err != CHDERR_NONE)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "CHD: cannot open %s: %s\n", path, chd_error_string(err));
      st->chd = NULL;
      return false;
   }
   hdr = chd_get_header(st->chd);
   if (chd_get_metadata(st->chd, DVD_METADATA_TAG, 0, meta, sizeof(meta), &metalen,
                        NULL, NULL) != CHDERR_NONE)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "CHD: %s is not a DVD image (no 'DVD ' metadata; "
             "create it with 'chdman createdvd')\n", path);
      return false;
   }
   if (!hdr || hdr->hunkbytes == 0 || hdr->hunkbytes % 2048)
   {
      dv_log(ctx, RETRO_LOG_ERROR, "CHD: unexpected hunk size %u\n", hdr ? hdr->hunkbytes : 0);
      return false;
   }
   st->hunkbytes  = hdr->hunkbytes;
   st->size       = hdr->logicalbytes;
   st->hunk       = malloc(st->hunkbytes);
   st->cached     = -1;
   if (!st->hunk)
      return false;
   dv_log(ctx, RETRO_LOG_INFO, "CHD: v%u, %llu bytes (%llu sectors), hunk %u bytes\n",
          hdr->version, (unsigned long long)st->size,
          (unsigned long long)(st->size / 2048), st->hunkbytes);
   return true;
}

static int chd_stream_read(dv_stream_t *st, uint8_t *buf, int len)
{
   int done = 0;
   while (done < len && st->pos < st->size)
   {
      int64_t hunk = (int64_t)(st->pos / st->hunkbytes);
      uint32_t off = (uint32_t)(st->pos % st->hunkbytes);
      uint32_t n   = st->hunkbytes - off;

      if (hunk != st->cached)
      {
         if (chd_read(st->chd, (uint32_t)hunk, st->hunk) != CHDERR_NONE)
            return done ? done : -1;
         st->cached = hunk;
      }
      if (n > (uint32_t)(len - done))
         n = (uint32_t)(len - done);
      if (n > st->size - st->pos)
         n = (uint32_t)(st->size - st->pos);
      memcpy(buf + done, st->hunk + off, n);
      done    += (int)n;
      st->pos += n;
   }
   return done;
}

/* ---- libdvdread stream callbacks -------------------------------------- */

/* libdvdread contract (dvd_input.c): pf_seek returns 0 on success, -1 on
 * error; pf_read returns bytes read (<0 on error). */

static int stream_seek(void *priv, uint64_t pos)
{
   dv_stream_t *st = priv;
   if (st->chd)
   {
      if (pos > st->size)
         return -1;
      st->pos = pos;
      return 0;
   }
   return raw_seek(st, (int64_t)pos, SEEK_SET);
}

static int stream_read(void *priv, void *buf, int len)
{
   dv_stream_t *st = priv;
   if (len <= 0)
      return 0;
   if (st->chd)
      return chd_stream_read(st, buf, len);
   return (int)raw_read(st, buf, (uint64_t)len);
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
      if (!st->vfs_fh)
         dv_log(ctx, RETRO_LOG_WARN, "VFS open failed, falling back to stdio: %s\n",
                src->path);
   }
   if (!st->vfs_fh)
   {
      st->fp = fopen(src->path, "rb");
      if (!st->fp)
      {
         dv_log(ctx, RETRO_LOG_ERROR, "Cannot open %s: %s\n", src->path, strerror(errno));
         return false;
      }
   }

   if (src->is_chd && !chd_attach(ctx, st, src->path))
   {
      dv_stream_close(st);
      return false;
   }
   return true;
}

void dv_stream_close(dv_stream_t *st)
{
   if (st->chd)
      chd_close(st->chd);   /* our fclose callback is a no-op */
   st->chd = NULL;
   free(st->hunk);
   st->hunk = NULL;
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

/* libdvdcss reads DVDCSS_METHOD at open (D-003: env only, never direct
 * calls). Unset = its default ("key"). */
void dv_source_setup_css_method(dv_ctx_t *ctx)
{
   if (!ctx->opt.css_method[0])
      return;
#ifdef _WIN32
   _putenv_s("DVDCSS_METHOD", ctx->opt.css_method);
#else
   setenv("DVDCSS_METHOD", ctx->opt.css_method, 1);
#endif
   dv_log(ctx, RETRO_LOG_INFO, "DVDCSS_METHOD=%s\n", ctx->opt.css_method);
}
