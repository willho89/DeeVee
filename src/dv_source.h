/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Content resolution + stream I/O for libdvdread (CLAUDE.md §5.1).
 */
#ifndef DV_SOURCE_H
#define DV_SOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <dvdread/dvd_reader.h>

struct dv_ctx;
struct retro_vfs_file_handle;

typedef enum
{
   DV_SOURCE_NONE = 0,
   DV_SOURCE_IMAGE,     /* .iso/.img opened through stream callbacks */
   DV_SOURCE_DIRECTORY  /* VIDEO_TS folder / mount root, path handed to libdvdread */
} dv_source_kind_t;

/* One open handle on an image file. libdvdread keeps its own read position
 * per dvd_reader_t, so every reader (nav + probes) needs its own handle. */
typedef struct dv_stream
{
   struct dv_ctx *ctx;
   struct retro_vfs_file_handle *vfs_fh;  /* when the frontend VFS is used */
   FILE *fp;                              /* plain-path fallback */
} dv_stream_t;

typedef struct dv_source
{
   dv_source_kind_t kind;
   char path[4096];          /* image file, or directory for libdvdread */
} dv_source_t;

/* Classify the content path and fill src->path. Returns false on error. */
bool dv_source_resolve(struct dv_ctx *ctx, const char *content_path, dv_source_t *src);

/* Open/close a stream handle on an image source and get libdvdread callbacks. */
bool dv_stream_open(struct dv_ctx *ctx, const dv_source_t *src, dv_stream_t *st);
void dv_stream_close(dv_stream_t *st);
dvd_reader_stream_cb *dv_stream_callbacks(void);

/* Point libdvdcss's key cache at <system>/deevee/dvdcss (env var only; D-003). */
void dv_source_setup_css_env(struct dv_ctx *ctx);

#endif
