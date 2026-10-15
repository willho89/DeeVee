/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared core context. No globals outside libretro.c: every module takes a
 * dv_ctx_t* (CLAUDE.md §10).
 */
#ifndef DV_CTX_H
#define DV_CTX_H

#include <stdbool.h>
#include <stdint.h>
#include <libretro.h>

#include "dv_source.h"
#include "dv_nav.h"

#define DV_VERSION "0.1.0"

#ifndef DEEVEE_CSS_MODE
#define DEEVEE_CSS_MODE "unknown"
#endif

typedef struct dv_ctx
{
   retro_environment_t     environ_cb;
   retro_log_printf_t      log_cb;
   struct retro_vfs_interface *vfs;   /* NULL if frontend has no VFS v1+ */

   char system_dir[4096];

   dv_source_t source;
   dv_nav_t    nav;
} dv_ctx_t;

void dv_log(dv_ctx_t *ctx, enum retro_log_level level, const char *fmt, ...)
#ifdef __GNUC__
   __attribute__((format(printf, 3, 4)))
#endif
   ;

#endif
