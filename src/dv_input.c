/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Input mapping (CLAUDE.md §5.9). Buttons are edge-triggered; the D-pad
 * auto-repeats after 400 ms. When the VOBU on screen has buttons, the D-pad
 * and A drive them; otherwise they're title controls (seek / pause: M6).
 * Pointer (mouse/touch) maps the frontend's [-0x7fff, 0x7fff] image range
 * to stored-frame coordinates for dvdnav_mouse_select/activate.
 */
#include <string.h>

#include "dv_ctx.h"
#include "dv_input.h"

#define REPEAT_DELAY_MS  400
#define REPEAT_PERIOD_MS 100

void dv_input_init(dv_ctx_t *ctx, retro_input_state_t cb, bool bitmasks)
{
   memset(&ctx->input, 0, sizeof(ctx->input));
   ctx->input.state_cb = cb;
   ctx->input.bitmasks = bitmasks;
}

void dv_input_set_descriptors(retro_environment_t environ_cb)
{
   static const struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Menu Up / Seek +60 s" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Menu Down / Seek -60 s" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Menu Left / Seek -10 s" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Menu Right / Seek +10 s" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Select Button / Play-Pause" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Return" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Root Menu" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Title Menu / Info" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Previous Chapter" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Chapter" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Audio Track" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Subtitles" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Rewind (hold)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward (hold)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Angle" },
      { 0, 0, 0, 0, NULL },
   };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

static uint32_t read_buttons(dv_input_t *in)
{
   uint32_t bits = 0;
   unsigned id;
   if (in->bitmasks)
      return (uint32_t)in->state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
   for (id = 0; id < 16; id++)
      if (in->state_cb(0, RETRO_DEVICE_JOYPAD, 0, id))
         bits |= 1u << id;
   return bits;
}

/* True on press, and periodically while held (auto-repeat). */
static bool fire(dv_ctx_t *ctx, uint32_t bits, unsigned id, bool repeat)
{
   dv_input_t *in = &ctx->input;
   unsigned delay  = (unsigned)(REPEAT_DELAY_MS * ctx->clock.fps / 1000.0 + 0.5);
   unsigned period = (unsigned)(REPEAT_PERIOD_MS * ctx->clock.fps / 1000.0 + 0.5);
   bool down = (bits >> id) & 1;
   bool pressed = down && !((in->prev >> id) & 1);

   if (!down)
   {
      in->held_ticks[id] = 0;
      return false;
   }
   in->held_ticks[id]++;
   if (pressed)
      return true;
   return repeat && period && in->held_ticks[id] > delay &&
          (in->held_ticks[id] - delay) % period == 0;
}

static void run_pointer(dv_ctx_t *ctx)
{
   dv_input_t *in = &ctx->input;
   int16_t px = in->state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
   int16_t py = in->state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
   bool pressed = in->state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
   unsigned ref_h = (ctx->render.height == 576 || ctx->render.height == 288) ? 576 : 480;
   int x, y;

   /* [-0x7fff, 0x7fff] spans the displayed image; the frontend has already
    * undone aspect scaling and letterboxing. Map to the 720-wide stored
    * frame that button coordinates use. */
   x = (int)(((int32_t)px + 0x7fff) * 720 / 0xfffe);
   y = (int)(((int32_t)py + 0x7fff) * (int32_t)ref_h / 0xfffe);

   /* Hover selects (only when it actually moved, so a still mouse doesn't
    * fight the D-pad); press activates. */
   if (!in->ptr_valid || x != in->ptr_x || y != in->ptr_y)
   {
      if (in->ptr_valid && (px || py))
         dv_nav_pointer(ctx, x, y, false);
      in->ptr_x = x;
      in->ptr_y = y;
      in->ptr_valid = true;
   }
   if (pressed && !in->ptr_pressed)
   {
      dv_nav_pointer(ctx, x, y, false);
      dv_nav_pointer(ctx, x, y, true);
   }
   in->ptr_pressed = pressed;
}

#define SEEK_SHORT (90000 * 10)
#define SEEK_LONG  (90000 * 60)
#define SCAN_STEP  (90000 * 15)   /* L2/R2 held: jump this far... */
#define SCAN_MS    300            /* ...this often */

void dv_input_run(dv_ctx_t *ctx)
{
   dv_input_t *in = &ctx->input;
   uint32_t bits;
   bool title;

   if (!in->state_cb)
      return;
   bits  = read_buttons(in);
   title = dv_nav_in_title(ctx);   /* else: menu (buttons) or still */

   /* D-pad: buttons in menus; seek in titles (§5.9). */
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_UP, true))
      title ? dv_nav_seek(ctx, SEEK_LONG) : dv_nav_button_move(ctx, DV_BTN_UP);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_DOWN, true))
      title ? dv_nav_seek(ctx, -SEEK_LONG) : dv_nav_button_move(ctx, DV_BTN_DOWN);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_LEFT, true))
      title ? dv_nav_seek(ctx, -SEEK_SHORT) : dv_nav_button_move(ctx, DV_BTN_LEFT);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_RIGHT, true))
      title ? dv_nav_seek(ctx, SEEK_SHORT) : dv_nav_button_move(ctx, DV_BTN_RIGHT);

   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_A, false))
      dv_nav_action(ctx, DV_ACT_ACTIVATE);   /* button / skip still / pause */
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_B, false))
      dv_nav_action(ctx, DV_ACT_GO_UP);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_START, false))
      dv_nav_action(ctx, DV_ACT_ROOT_MENU);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_SELECT, false))
      dv_nav_action(ctx, title ? DV_ACT_INFO : DV_ACT_TITLE_MENU);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_L, false))
      dv_nav_action(ctx, DV_ACT_PREV_CHAPTER);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_R, false))
      dv_nav_action(ctx, DV_ACT_NEXT_CHAPTER);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_X, false))
      dv_nav_action(ctx, DV_ACT_CYCLE_AUDIO);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_Y, false))
      dv_nav_action(ctx, DV_ACT_CYCLE_SUBS);
   if (fire(ctx, bits, RETRO_DEVICE_ID_JOYPAD_L3, false))
      dv_nav_action(ctx, DV_ACT_CYCLE_ANGLE);

   /* L2/R2 held: scan by repeated jumps (sector-accurate, shows pictures
    * on the way, unlike a time-stretched fast forward). */
   if (title)
   {
      unsigned every = (unsigned)(SCAN_MS * ctx->clock.fps / 1000.0 + 0.5);
      unsigned id;
      for (id = RETRO_DEVICE_ID_JOYPAD_L2; id <= RETRO_DEVICE_ID_JOYPAD_R2; id++)
      {
         bool down = (bits >> id) & 1;
         if (!down)
            in->held_ticks[id] = 0;
         else if (in->held_ticks[id]++ % every == 0)
            dv_nav_seek(ctx, id == RETRO_DEVICE_ID_JOYPAD_R2 ? SCAN_STEP : -SCAN_STEP);
      }
   }

   in->prev = bits;
   run_pointer(ctx);
}
