#include "deevee_dvdnav.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef HAVE_DVDNAV
#define HAVE_DVDNAV 0
#endif

#if HAVE_DVDNAV
#include <dvdnav/dvdnav.h>
#endif

#if HAVE_DVDNAV
static int dvdnav_stream_seek(void *stream, uint64_t pos)
{
   struct deevee_dvdnav *nav = (struct deevee_dvdnav *)stream;

   if (!nav || pos > nav->stream_size)
      return -1;

   nav->position = pos;
   return 0;
}

static int dvdnav_stream_read(void *stream, void *buffer, int bytes_to_read)
{
   struct deevee_dvdnav *nav = (struct deevee_dvdnav *)stream;
   uint8_t *out = (uint8_t *)buffer;
   int bytes_read = 0;
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];

   if (!nav || !buffer || bytes_to_read < 0)
      return -1;

   while (bytes_read < bytes_to_read && nav->position < nav->stream_size)
   {
      uint64_t lba = nav->position / DEEVEE_DVD_SECTOR_SIZE;
      size_t sector_offset = (size_t)(nav->position % DEEVEE_DVD_SECTOR_SIZE);
      size_t available = DEEVEE_DVD_SECTOR_SIZE - sector_offset;
      size_t requested = (size_t)(bytes_to_read - bytes_read);
      size_t remaining = (size_t)(nav->stream_size - nav->position);
      size_t to_copy = available;

      if (to_copy > requested)
         to_copy = requested;
      if (to_copy > remaining)
         to_copy = remaining;

      if (deevee_disc_read_sector(&nav->disc, lba, sector, sizeof(sector)) !=
            DEEVEE_DISC_OK)
         return bytes_read ? bytes_read : -1;

      memcpy(out + bytes_read, sector + sector_offset, to_copy);
      nav->position += to_copy;
      bytes_read += (int)to_copy;
   }

   return bytes_read;
}

static void dvdnav_log(void *user_data, dvdnav_logger_level_t level,
      const char *format, va_list args)
{
   struct deevee_dvdnav *nav = (struct deevee_dvdnav *)user_data;

   (void)level;
   if (!nav || !format)
      return;

   vsnprintf(nav->last_error, sizeof(nav->last_error), format, args);
}
#endif

void deevee_dvdnav_init(struct deevee_dvdnav *nav)
{
   if (!nav)
      return;

   memset(nav, 0, sizeof(*nav));
   deevee_disc_init(&nav->disc);
}

void deevee_dvdnav_close(struct deevee_dvdnav *nav)
{
   if (!nav)
      return;

#if HAVE_DVDNAV
   if (nav->handle)
      dvdnav_close((dvdnav_t *)nav->handle);
#endif
   deevee_disc_close(&nav->disc);
   deevee_dvdnav_init(nav);
}

enum deevee_dvdnav_status deevee_dvdnav_open(struct deevee_dvdnav *nav,
      const struct deevee_content_info *content)
{
#if HAVE_DVDNAV
   dvdnav_t *handle = NULL;
   dvdnav_stream_cb stream_cb;
   dvdnav_logger_cb logger;

   if (!nav || !content)
      return DEEVEE_DVDNAV_ERROR_INVALID_ARGUMENT;

   deevee_dvdnav_close(nav);
   nav->content = *content;
   nav->stream_size = 0;
   nav->position = 0;

   if (deevee_disc_open(&nav->disc, content) != DEEVEE_DISC_OK)
      return DEEVEE_DVDNAV_ERROR_OPEN_FAILED;

   nav->stream_size = nav->disc.sector_count * DEEVEE_DVD_SECTOR_SIZE;
   memset(&stream_cb, 0, sizeof(stream_cb));
   stream_cb.pf_seek = dvdnav_stream_seek;
   stream_cb.pf_read = dvdnav_stream_read;
   stream_cb.pf_readv = NULL;
   memset(&logger, 0, sizeof(logger));
   logger.pf_log = dvdnav_log;

   if (dvdnav_open_stream2(&handle, nav, &logger, &stream_cb) !=
         DVDNAV_STATUS_OK)
   {
      snprintf(nav->last_error, sizeof(nav->last_error), "%s",
            handle ? dvdnav_err_to_string(handle) : "dvdnav_open_stream2 failed");
      if (handle)
         dvdnav_close(handle);
      deevee_disc_close(&nav->disc);
      return DEEVEE_DVDNAV_ERROR_OPEN_FAILED;
   }

   nav->handle = handle;
   dvdnav_set_readahead_flag(handle, 0);
   dvdnav_set_PGC_positioning_flag(handle, 1);
   return DEEVEE_DVDNAV_OK;
#else
   (void)nav;
   (void)content;
   return DEEVEE_DVDNAV_ERROR_UNSUPPORTED;
#endif
}

enum deevee_dvdnav_status deevee_dvdnav_next(struct deevee_dvdnav *nav,
      struct deevee_dvdnav_event *event)
{
#if HAVE_DVDNAV
   int32_t nav_event = 0;
   int32_t length = 0;

   if (!nav || !nav->handle || !event)
      return DEEVEE_DVDNAV_ERROR_INVALID_ARGUMENT;

   memset(event, 0, sizeof(*event));
   if (dvdnav_get_next_block((dvdnav_t *)nav->handle, event->data,
            &nav_event, &length) != DVDNAV_STATUS_OK)
   {
      snprintf(nav->last_error, sizeof(nav->last_error), "%s",
            dvdnav_err_to_string((dvdnav_t *)nav->handle));
      return DEEVEE_DVDNAV_ERROR_READ_FAILED;
   }

   event->event = nav_event;
   event->length = length;
   if (nav_event == DVDNAV_SPU_CLUT_CHANGE && length >= 64)
   {
      memcpy(nav->current_spu_clut, event->data,
            sizeof(nav->current_spu_clut));
      nav->has_current_spu_clut = true;
   }
   return DEEVEE_DVDNAV_OK;
#else
   (void)nav;
   (void)event;
   return DEEVEE_DVDNAV_ERROR_UNSUPPORTED;
#endif
}

bool deevee_dvdnav_menu_call_root(struct deevee_dvdnav *nav)
{
#if HAVE_DVDNAV
   return nav && nav->handle &&
      dvdnav_menu_call((dvdnav_t *)nav->handle, DVD_MENU_Root) ==
         DVDNAV_STATUS_OK;
#else
   (void)nav;
   return false;
#endif
}

bool deevee_dvdnav_previous_chapter(struct deevee_dvdnav *nav)
{
#if HAVE_DVDNAV
   int32_t title = 0;
   int32_t part = 0;

   if (!nav || !nav->handle)
      return false;

   if (dvdnav_current_title_info((dvdnav_t *)nav->handle, &title, &part) ==
         DVDNAV_STATUS_OK && title > 0 && part > 1)
      return dvdnav_part_search((dvdnav_t *)nav->handle, part - 1) ==
         DVDNAV_STATUS_OK;

   return dvdnav_prev_pg_search((dvdnav_t *)nav->handle) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   return false;
#endif
}

bool deevee_dvdnav_next_chapter(struct deevee_dvdnav *nav)
{
#if HAVE_DVDNAV
   int32_t title = 0;
   int32_t part = 0;
   int32_t parts = 0;

   if (!nav || !nav->handle)
      return false;

   if (dvdnav_current_title_info((dvdnav_t *)nav->handle, &title, &part) ==
         DVDNAV_STATUS_OK && title > 0 &&
         dvdnav_get_number_of_parts((dvdnav_t *)nav->handle, title, &parts) ==
            DVDNAV_STATUS_OK && part > 0 && part < parts)
      return dvdnav_part_search((dvdnav_t *)nav->handle, part + 1) ==
         DVDNAV_STATUS_OK;

   return dvdnav_next_pg_search((dvdnav_t *)nav->handle) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   return false;
#endif
}

bool deevee_dvdnav_scan_seconds(struct deevee_dvdnav *nav, int seconds)
{
#if HAVE_DVDNAV
   int64_t current_time;
   int64_t target_time;

   if (!nav || !nav->handle || !seconds)
      return false;

   current_time = dvdnav_get_current_time((dvdnav_t *)nav->handle);
   if (current_time < 0)
      return false;

   target_time = current_time + (int64_t)seconds * 90000;
   if (target_time < 0)
      target_time = 0;

   return dvdnav_time_search((dvdnav_t *)nav->handle,
      (uint64_t)target_time) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   (void)seconds;
   return false;
#endif
}

bool deevee_dvdnav_play_title_part(struct deevee_dvdnav *nav, int title,
      int part)
{
#if HAVE_DVDNAV
   if (!nav || !nav->handle || title <= 0)
      return false;
   if (part <= 0)
      part = 1;

   return dvdnav_part_play((dvdnav_t *)nav->handle, title, part) ==
      DVDNAV_STATUS_OK;
#else
   (void)nav;
   (void)title;
   (void)part;
   return false;
#endif
}

bool deevee_dvdnav_read_position(struct deevee_dvdnav *nav, int *title,
      int *part, int *parts, int64_t *time_ticks)
{
#if HAVE_DVDNAV
   int32_t current_title = 0;
   int32_t current_part = 0;
   int32_t current_parts = 0;

   if (title)
      *title = 0;
   if (part)
      *part = 0;
   if (parts)
      *parts = 0;
   if (time_ticks)
      *time_ticks = -1;

   if (!nav || !nav->handle)
      return false;

   if (dvdnav_current_title_info((dvdnav_t *)nav->handle, &current_title,
            &current_part) != DVDNAV_STATUS_OK)
      return false;

   if (current_title > 0)
      (void)dvdnav_get_number_of_parts((dvdnav_t *)nav->handle,
            current_title, &current_parts);

   if (title)
      *title = current_title;
   if (part)
      *part = current_part;
   if (parts)
      *parts = current_parts;
   if (time_ticks)
      *time_ticks = dvdnav_get_current_time((dvdnav_t *)nav->handle);

   return true;
#else
   (void)nav;
   if (title)
      *title = 0;
   if (part)
      *part = 0;
   if (parts)
      *parts = 0;
   if (time_ticks)
      *time_ticks = -1;
   return false;
#endif
}

int deevee_dvdnav_stream_count(struct deevee_dvdnav *nav, bool audio)
{
#if HAVE_DVDNAV
   if (!nav || !nav->handle)
      return 0;

   return dvdnav_get_number_of_streams((dvdnav_t *)nav->handle,
      audio ? DVD_AUDIO_STREAM : DVD_SUBTITLE_STREAM);
#else
   (void)nav;
   (void)audio;
   return 0;
#endif
}

int deevee_dvdnav_active_stream(struct deevee_dvdnav *nav, bool audio)
{
#if HAVE_DVDNAV
   if (!nav || !nav->handle)
      return -1;

   return audio ? dvdnav_get_active_audio_stream((dvdnav_t *)nav->handle) :
      dvdnav_get_active_spu_stream((dvdnav_t *)nav->handle);
#else
   (void)nav;
   (void)audio;
   return -1;
#endif
}

bool deevee_dvdnav_set_active_stream(struct deevee_dvdnav *nav, bool audio,
      int stream)
{
#if HAVE_DVDNAV
   if (!nav || !nav->handle || stream < 0)
      return false;

   return dvdnav_set_active_stream((dvdnav_t *)nav->handle, (uint8_t)stream,
      audio ? DVD_AUDIO_STREAM : DVD_SUBTITLE_STREAM) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   (void)audio;
   (void)stream;
   return false;
#endif
}

bool deevee_dvdnav_set_spu_visible(struct deevee_dvdnav *nav, bool visible)
{
#if HAVE_DVDNAV
   if (!nav || !nav->handle)
      return false;

   return dvdnav_toggle_spu_stream((dvdnav_t *)nav->handle,
      visible ? 1 : 0) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   (void)visible;
   return false;
#endif
}

uint16_t deevee_dvdnav_stream_language(struct deevee_dvdnav *nav, bool audio,
      int logical_stream)
{
#if HAVE_DVDNAV
   if (!nav || !nav->handle || logical_stream < 0)
      return 0xffffu;

   return audio ?
      dvdnav_audio_stream_to_lang((dvdnav_t *)nav->handle,
         (uint8_t)logical_stream) :
      dvdnav_spu_stream_to_lang((dvdnav_t *)nav->handle,
         (uint8_t)logical_stream);
#else
   (void)nav;
   (void)audio;
   (void)logical_stream;
   return 0xffffu;
#endif
}

bool deevee_dvdnav_stream_metadata(struct deevee_dvdnav *nav, bool audio,
      int logical_stream, struct deevee_dvdnav_stream_metadata *metadata)
{
#if HAVE_DVDNAV
   dvdnav_t *handle;

   if (!nav || !nav->handle || logical_stream < 0 || !metadata)
      return false;

   memset(metadata, 0, sizeof(*metadata));
   handle = (dvdnav_t *)nav->handle;
   metadata->language = audio ?
      dvdnav_audio_stream_to_lang(handle, (uint8_t)logical_stream) :
      dvdnav_spu_stream_to_lang(handle, (uint8_t)logical_stream);
   metadata->format = 0xffffu;
   metadata->channels = 0xffffu;

   if (audio)
   {
      audio_attr_t attr;

      metadata->format = dvdnav_audio_stream_format(handle,
            (uint8_t)logical_stream);
      metadata->channels = dvdnav_audio_stream_channels(handle,
            (uint8_t)logical_stream);
      if (dvdnav_get_audio_attr(handle, (uint8_t)logical_stream, &attr) ==
            DVDNAV_STATUS_OK)
      {
         metadata->code_extension = attr.code_extension;
         metadata->has_code_extension = true;
      }
   }
   else
   {
      subp_attr_t attr;

      if (dvdnav_get_spu_attr(handle, (uint8_t)logical_stream, &attr) ==
            DVDNAV_STATUS_OK)
      {
         metadata->code_extension = attr.code_extension;
         metadata->has_code_extension = true;
      }
   }

   return true;
#else
   (void)nav;
   (void)audio;
   (void)logical_stream;
   (void)metadata;
   return false;
#endif
}

bool deevee_dvdnav_read_audio_stream_change(
      const struct deevee_dvdnav_event *event,
      struct deevee_dvdnav_audio_stream_change *change)
{
#if HAVE_DVDNAV
   const dvdnav_audio_stream_change_event_t *dvd_change;

   if (!event || !change || event->event != DVDNAV_AUDIO_STREAM_CHANGE ||
         event->length < (int)sizeof(*dvd_change))
      return false;

   dvd_change = (const dvdnav_audio_stream_change_event_t *)event->data;
   change->physical = dvd_change->physical;
   change->logical = dvd_change->logical;
   return true;
#else
   (void)event;
   (void)change;
   return false;
#endif
}

bool deevee_dvdnav_read_spu_stream_change(
      const struct deevee_dvdnav_event *event,
      struct deevee_dvdnav_spu_stream_change *change)
{
#if HAVE_DVDNAV
   const dvdnav_spu_stream_change_event_t *dvd_change;

   if (!event || !change || event->event != DVDNAV_SPU_STREAM_CHANGE ||
         event->length < (int)sizeof(*dvd_change))
      return false;

   dvd_change = (const dvdnav_spu_stream_change_event_t *)event->data;
   change->physical_wide = dvd_change->physical_wide;
   change->physical_letterbox = dvd_change->physical_letterbox;
   change->physical_pan_scan = dvd_change->physical_pan_scan;
   change->logical = dvd_change->logical;
   return true;
#else
   (void)event;
   (void)change;
   return false;
#endif
}

bool deevee_dvdnav_select_button(struct deevee_dvdnav *nav, int button)
{
#if HAVE_DVDNAV
   pci_t *pci;

   if (!nav || !nav->handle || button <= 0)
      return false;

   pci = dvdnav_get_current_nav_pci((dvdnav_t *)nav->handle);
   return pci && dvdnav_button_select((dvdnav_t *)nav->handle, pci, button) ==
      DVDNAV_STATUS_OK;
#else
   (void)nav;
   (void)button;
   return false;
#endif
}

bool deevee_dvdnav_activate_button(struct deevee_dvdnav *nav, int button)
{
#if HAVE_DVDNAV
   pci_t *pci;

   if (!nav || !nav->handle || button <= 0)
      return false;

   pci = dvdnav_get_current_nav_pci((dvdnav_t *)nav->handle);
   return pci && dvdnav_button_select_and_activate((dvdnav_t *)nav->handle,
      pci, button) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   (void)button;
   return false;
#endif
}

bool deevee_dvdnav_button(struct deevee_dvdnav *nav,
      enum deevee_dvdnav_button button)
{
#if HAVE_DVDNAV
   pci_t *pci;
   dvdnav_t *handle;

   if (!nav || !nav->handle)
      return false;

   handle = (dvdnav_t *)nav->handle;
   pci = dvdnav_get_current_nav_pci(handle);
   if (!pci)
      return false;

   switch (button)
   {
      case DEEVEE_DVDNAV_BUTTON_UP:
         return dvdnav_upper_button_select(handle, pci) == DVDNAV_STATUS_OK;
      case DEEVEE_DVDNAV_BUTTON_DOWN:
         return dvdnav_lower_button_select(handle, pci) == DVDNAV_STATUS_OK;
      case DEEVEE_DVDNAV_BUTTON_LEFT:
         return dvdnav_left_button_select(handle, pci) == DVDNAV_STATUS_OK;
      case DEEVEE_DVDNAV_BUTTON_RIGHT:
         return dvdnav_right_button_select(handle, pci) == DVDNAV_STATUS_OK;
      case DEEVEE_DVDNAV_BUTTON_ACTIVATE:
         return dvdnav_button_activate(handle, pci) == DVDNAV_STATUS_OK;
      default:
         return false;
   }
#else
   (void)nav;
   (void)button;
   return false;
#endif
}

bool deevee_dvdnav_read_buttons(struct deevee_dvdnav *nav,
      struct deevee_dvd_menu_button *buttons, uint8_t *button_count,
      uint8_t *active_button, uint32_t *select_color_table,
      bool *has_select_color_table, uint32_t *spu_clut,
      bool *has_spu_clut)
{
#if HAVE_DVDNAV
   pci_t *pci;
   int32_t highlight = 0;
   uint8_t display_types[3];
   uint8_t group_count;
   uint8_t group_index = 0;
   uint8_t buttons_per_group;
   uint8_t video_aspect;
   uint8_t count;
   uint8_t i;

   if (!nav || !nav->handle || !buttons || !button_count || !active_button)
      return false;

   pci = dvdnav_get_current_nav_pci((dvdnav_t *)nav->handle);
   if (!pci)
      return false;

   group_count = pci->hli.hl_gi.btngr_ns & 0x03u;
   if (!group_count || group_count > 3u)
      group_count = 1u;
   buttons_per_group = (uint8_t)(36u / group_count);
   display_types[0] = pci->hli.hl_gi.btngr1_dsp_ty & 0x07u;
   display_types[1] = pci->hli.hl_gi.btngr2_dsp_ty & 0x07u;
   display_types[2] = pci->hli.hl_gi.btngr3_dsp_ty & 0x07u;
   video_aspect = dvdnav_get_video_aspect((dvdnav_t *)nav->handle);

   for (i = 0; i < group_count; i++)
   {
      if ((video_aspect == 2u && (display_types[i] & 0x01u)) ||
            (video_aspect != 2u && display_types[i] == 0u))
      {
         group_index = i;
         break;
      }
   }

   count = pci->hli.hl_gi.btn_ns & 0x3fu;
   if (count > buttons_per_group)
      count = buttons_per_group;
   if (count > DEEVEE_DVD_MAX_MENU_BUTTONS)
      count = DEEVEE_DVD_MAX_MENU_BUTTONS;

   memset(buttons, 0, DEEVEE_DVD_MAX_MENU_BUTTONS * sizeof(buttons[0]));
   for (i = 0; i < count; i++)
   {
      const btni_t *source = &pci->hli.btnit[
         (size_t)group_index * buttons_per_group + i];
      struct deevee_dvd_menu_button *target = &buttons[i];

      target->number = i + 1u;
      target->color_table = (uint8_t)source->btn_coln;
      target->auto_action = source->auto_action_mode != 0;
      target->x_start = (uint16_t)source->x_start;
      target->x_end = (uint16_t)source->x_end;
      target->y_start = (uint16_t)source->y_start;
      target->y_end = (uint16_t)source->y_end;
      target->up = (uint8_t)source->up;
      target->down = (uint8_t)source->down;
      target->left = (uint8_t)source->left;
      target->right = (uint8_t)source->right;
      memcpy(target->command, source->cmd.bytes, sizeof(target->command));
   }

   if (dvdnav_get_current_highlight((dvdnav_t *)nav->handle, &highlight) !=
         DVDNAV_STATUS_OK)
      highlight = pci->hli.hl_gi.fosl_btnn & 0x3f;

   if (select_color_table)
   {
      for (i = 0; i < 3u; i++)
         select_color_table[i] = pci->hli.btn_colit.btn_coli[i][0];
      if (has_select_color_table)
         *has_select_color_table = true;
   }
   else if (has_select_color_table)
      *has_select_color_table = false;

   if (spu_clut && nav->has_current_spu_clut)
      memcpy(spu_clut, nav->current_spu_clut, 16u * sizeof(spu_clut[0]));
   if (has_spu_clut)
      *has_spu_clut = nav->has_current_spu_clut;

   *button_count = count;
   *active_button = highlight > 0 && highlight <= count ? (uint8_t)highlight :
      (count ? 1u : 0u);
   return count > 0;
#else
   (void)nav;
   (void)buttons;
   (void)button_count;
   (void)active_button;
   (void)select_color_table;
   (void)has_select_color_table;
   (void)spu_clut;
   (void)has_spu_clut;
   return false;
#endif
}

bool deevee_dvdnav_read_highlight(struct deevee_dvdnav *nav,
      struct deevee_dvdnav_highlight *highlight, bool action_mode)
{
#if HAVE_DVDNAV
   pci_t *pci;
   int32_t button = 0;
   dvdnav_highlight_area_t area;

   if (!nav || !nav->handle || !highlight)
      return false;

   pci = dvdnav_get_current_nav_pci((dvdnav_t *)nav->handle);
   if (!pci)
      return false;

   if (dvdnav_get_current_highlight((dvdnav_t *)nav->handle, &button) !=
         DVDNAV_STATUS_OK || button <= 0)
      return false;

   if (dvdnav_get_highlight_area(pci, button, action_mode ? 1 : 0, &area) !=
         DVDNAV_STATUS_OK)
      return false;

   highlight->x_start = area.sx;
   highlight->x_end = area.ex;
   highlight->y_start = area.sy;
   highlight->y_end = area.ey;
   highlight->palette = area.palette;
   highlight->pts = area.pts;
   highlight->button = area.buttonN;
   return true;
#else
   (void)nav;
   (void)highlight;
   (void)action_mode;
   return false;
#endif
}

bool deevee_dvdnav_ack_event(struct deevee_dvdnav *nav, int event)
{
#if HAVE_DVDNAV
   dvdnav_t *handle;

   if (!nav || !nav->handle)
      return false;

   handle = (dvdnav_t *)nav->handle;
   switch (event)
   {
      case DVDNAV_STILL_FRAME:
         return dvdnav_still_skip(handle) == DVDNAV_STATUS_OK;
      case DVDNAV_WAIT:
         return dvdnav_wait_skip(handle) == DVDNAV_STATUS_OK;
      default:
         return true;
   }
#else
   (void)nav;
   (void)event;
   return false;
#endif
}

const char *deevee_dvdnav_event_name(int event)
{
#if HAVE_DVDNAV
   switch (event)
   {
      case DVDNAV_BLOCK_OK:
         return "block";
      case DVDNAV_NOP:
         return "nop";
      case DVDNAV_STILL_FRAME:
         return "still";
      case DVDNAV_SPU_STREAM_CHANGE:
         return "spu_stream";
      case DVDNAV_AUDIO_STREAM_CHANGE:
         return "audio_stream";
      case DVDNAV_VTS_CHANGE:
         return "vts";
      case DVDNAV_CELL_CHANGE:
         return "cell";
      case DVDNAV_NAV_PACKET:
         return "nav";
      case DVDNAV_STOP:
         return "stop";
      case DVDNAV_HIGHLIGHT:
         return "highlight";
      case DVDNAV_SPU_CLUT_CHANGE:
         return "spu_clut";
      case DVDNAV_HOP_CHANNEL:
         return "hop";
      case DVDNAV_WAIT:
         return "wait";
      default:
         return "unknown";
   }
#else
   (void)event;
   return "unsupported";
#endif
}

const char *deevee_dvdnav_status_name(enum deevee_dvdnav_status status)
{
   switch (status)
   {
      case DEEVEE_DVDNAV_OK:
         return "ok";
      case DEEVEE_DVDNAV_ERROR_UNSUPPORTED:
         return "unsupported";
      case DEEVEE_DVDNAV_ERROR_INVALID_ARGUMENT:
         return "invalid_argument";
      case DEEVEE_DVDNAV_ERROR_OPEN_FAILED:
         return "open_failed";
      case DEEVEE_DVDNAV_ERROR_READ_FAILED:
         return "read_failed";
      default:
         return "unknown";
   }
}

const char *deevee_dvdnav_error(const struct deevee_dvdnav *nav)
{
   if (!nav || !nav->last_error[0])
      return "";

   return nav->last_error;
}

bool deevee_dvdnav_available(void)
{
   return HAVE_DVDNAV != 0;
}
