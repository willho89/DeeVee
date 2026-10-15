/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * RetroPad / pointer -> navigation (CLAUDE.md §5.9).
 */
#ifndef DV_INPUT_H
#define DV_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <libretro.h>

struct dv_ctx;

typedef struct dv_input
{
   retro_input_state_t state_cb;
   bool     bitmasks;        /* frontend supports RETRO_DEVICE_ID_JOYPAD_MASK */

   uint32_t prev;            /* joypad bits last tick */
   unsigned held_ticks[16];  /* how long each button has been held */

   int      ptr_x, ptr_y;    /* last pointer position, stored-frame coords */
   bool     ptr_valid;
   bool     ptr_pressed;
} dv_input_t;

void dv_input_init(struct dv_ctx *ctx, retro_input_state_t cb, bool bitmasks);

/* Poll and act; call once per retro_run after input_poll. */
void dv_input_run(struct dv_ctx *ctx);

/* Describe the controls to the frontend. */
void dv_input_set_descriptors(retro_environment_t environ_cb);

#endif
