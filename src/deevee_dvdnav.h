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
   uint32_t current_spu_clut[16];
   bool has_current_spu_clut;
   char last_error[256];
};

struct deevee_dvdnav_highlight
{
   uint16_t x_start;
   uint16_t x_end;
   uint16_t y_start;
   uint16_t y_end;
   uint32_t palette;
   uint32_t pts;
   uint32_t button;
};

struct deevee_dvdnav_audio_stream_change
{
   int physical;
   int logical;
};

struct deevee_dvdnav_spu_stream_change
{
   int physical_wide;
   int physical_letterbox;
   int physical_pan_scan;
   int logical;
};

struct deevee_dvdnav_stream_metadata
{
   uint16_t language;
   uint16_t format;
   uint16_t channels;
   uint8_t code_extension;
   bool has_code_extension;
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
bool deevee_dvdnav_scan_seconds(struct deevee_dvdnav *nav, int seconds);
bool deevee_dvdnav_play_title_part(struct deevee_dvdnav *nav, int title,
      int part);
bool deevee_dvdnav_read_position(struct deevee_dvdnav *nav, int *title,
      int *part, int *parts, int64_t *time_ticks);
int deevee_dvdnav_stream_count(struct deevee_dvdnav *nav, bool audio);
int deevee_dvdnav_active_stream(struct deevee_dvdnav *nav, bool audio);
bool deevee_dvdnav_set_active_stream(struct deevee_dvdnav *nav, bool audio,
      int stream);
bool deevee_dvdnav_set_spu_visible(struct deevee_dvdnav *nav, bool visible);
uint16_t deevee_dvdnav_stream_language(struct deevee_dvdnav *nav, bool audio,
      int logical_stream);
bool deevee_dvdnav_stream_metadata(struct deevee_dvdnav *nav, bool audio,
      int logical_stream, struct deevee_dvdnav_stream_metadata *metadata);
bool deevee_dvdnav_read_audio_stream_change(
      const struct deevee_dvdnav_event *event,
      struct deevee_dvdnav_audio_stream_change *change);
bool deevee_dvdnav_read_spu_stream_change(
      const struct deevee_dvdnav_event *event,
      struct deevee_dvdnav_spu_stream_change *change);
bool deevee_dvdnav_select_button(struct deevee_dvdnav *nav, int button);
bool deevee_dvdnav_activate_button(struct deevee_dvdnav *nav, int button);
bool deevee_dvdnav_button(struct deevee_dvdnav *nav,
      enum deevee_dvdnav_button button);
bool deevee_dvdnav_read_buttons(struct deevee_dvdnav *nav,
      struct deevee_dvd_menu_button *buttons, uint8_t *button_count,
      uint8_t *active_button, uint32_t *select_color_table,
      bool *has_select_color_table, uint32_t *spu_clut,
      bool *has_spu_clut);
bool deevee_dvdnav_read_highlight(struct deevee_dvdnav *nav,
      struct deevee_dvdnav_highlight *highlight, bool action_mode);
bool deevee_dvdnav_ack_event(struct deevee_dvdnav *nav, int event);
const char *deevee_dvdnav_event_name(int event);
const char *deevee_dvdnav_status_name(enum deevee_dvdnav_status status);
const char *deevee_dvdnav_error(const struct deevee_dvdnav *nav);
bool deevee_dvdnav_available(void);

#endif
