#ifndef DEEVEE_DVDNAV_H
#define DEEVEE_DVDNAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deevee_content.h"
#include "deevee_disc.h"
#include "deevee_dvd.h"

#define DEEVEE_DVDNAV_BLOCK_SIZE DEEVEE_DVD_SECTOR_SIZE

enum deevee_dvdnav_status
{
   DEEVEE_DVDNAV_OK = 0,
   DEEVEE_DVDNAV_ERROR_UNSUPPORTED,
   DEEVEE_DVDNAV_ERROR_INVALID_ARGUMENT,
   DEEVEE_DVDNAV_ERROR_OPEN_FAILED,
   DEEVEE_DVDNAV_ERROR_READ_FAILED
};

enum deevee_dvdnav_button
{
   DEEVEE_DVDNAV_BUTTON_UP = 0,
   DEEVEE_DVDNAV_BUTTON_DOWN,
   DEEVEE_DVDNAV_BUTTON_LEFT,
   DEEVEE_DVDNAV_BUTTON_RIGHT,
   DEEVEE_DVDNAV_BUTTON_ACTIVATE
};

struct deevee_dvdnav_event
{
   int event;
   int length;
   uint8_t data[DEEVEE_DVDNAV_BLOCK_SIZE];
};

struct deevee_dvdnav
{
   struct deevee_disc disc;
   struct deevee_content_info content;
   void *handle;
   uint64_t position;
   uint64_t stream_size;
   char last_error[256];
};

void deevee_dvdnav_init(struct deevee_dvdnav *nav);
void deevee_dvdnav_close(struct deevee_dvdnav *nav);
enum deevee_dvdnav_status deevee_dvdnav_open(struct deevee_dvdnav *nav,
      const struct deevee_content_info *content);
enum deevee_dvdnav_status deevee_dvdnav_next(struct deevee_dvdnav *nav,
      struct deevee_dvdnav_event *event);
bool deevee_dvdnav_menu_call_root(struct deevee_dvdnav *nav);
bool deevee_dvdnav_previous_chapter(struct deevee_dvdnav *nav);
bool deevee_dvdnav_next_chapter(struct deevee_dvdnav *nav);
bool deevee_dvdnav_select_button(struct deevee_dvdnav *nav, int button);
bool deevee_dvdnav_activate_button(struct deevee_dvdnav *nav, int button);
bool deevee_dvdnav_button(struct deevee_dvdnav *nav,
      enum deevee_dvdnav_button button);
bool deevee_dvdnav_read_buttons(struct deevee_dvdnav *nav,
      struct deevee_dvd_menu_button *buttons, uint8_t *button_count,
      uint8_t *active_button);
bool deevee_dvdnav_ack_event(struct deevee_dvdnav *nav, int event);
const char *deevee_dvdnav_event_name(int event);
const char *deevee_dvdnav_status_name(enum deevee_dvdnav_status status);
const char *deevee_dvdnav_error(const struct deevee_dvdnav *nav);
bool deevee_dvdnav_available(void);

#endif
