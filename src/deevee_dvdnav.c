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
   return nav && nav->handle &&
      dvdnav_prev_pg_search((dvdnav_t *)nav->handle) == DVDNAV_STATUS_OK;
#else
   (void)nav;
   return false;
#endif
}

bool deevee_dvdnav_next_chapter(struct deevee_dvdnav *nav)
{
#if HAVE_DVDNAV
   return nav && nav->handle &&
      dvdnav_next_pg_search((dvdnav_t *)nav->handle) == DVDNAV_STATUS_OK;
#else
   (void)nav;
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
      uint8_t *active_button)
{
#if HAVE_DVDNAV
   pci_t *pci;
   int32_t highlight = 0;
   uint8_t count;
   uint8_t i;

   if (!nav || !nav->handle || !buttons || !button_count || !active_button)
      return false;

   pci = dvdnav_get_current_nav_pci((dvdnav_t *)nav->handle);
   if (!pci)
      return false;

   count = pci->hli.hl_gi.btn_ns & 0x3fu;
   if (count > DEEVEE_DVD_MAX_MENU_BUTTONS)
      count = DEEVEE_DVD_MAX_MENU_BUTTONS;

   memset(buttons, 0, DEEVEE_DVD_MAX_MENU_BUTTONS * sizeof(buttons[0]));
   for (i = 0; i < count; i++)
   {
      const btni_t *source = &pci->hli.btnit[i];
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

   *button_count = count;
   *active_button = highlight > 0 && highlight <= count ? (uint8_t)highlight :
      (count ? 1u : 0u);
   return count > 0;
#else
   (void)nav;
   (void)buttons;
   (void)button_count;
   (void)active_button;
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
