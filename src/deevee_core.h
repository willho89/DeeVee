#ifndef DEEVEE_CORE_H
#define DEEVEE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deevee_audio.h"
#include "deevee_content.h"
#include "deevee_decoder.h"
#include "deevee_disc.h"
#include "deevee_dvd.h"
#include "deevee_dvdnav.h"
#include "deevee_nav.h"
#include "deevee_video.h"

#define DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY 8u

struct deevee_frame
{
   const void *pixels;
   unsigned width;
   unsigned height;
   size_t pitch;
};

struct deevee_audio_frame
{
   const int16_t *samples;
   size_t frames;
};

struct deevee_video_payload_chunk
{
   size_t offset;
   size_t size;
   bool has_pts;
   uint64_t pts;
   bool has_dts;
   uint64_t dts;
};

struct deevee_audio_payload_chunk
{
   size_t offset;
   size_t size;
};

struct deevee_decoded_video_frame
{
   uint32_t *pixels;
   bool valid;
   bool has_pts;
   int64_t pts;
   unsigned display_ticks;
   unsigned width;
   unsigned height;
   int pixel_format;
};

struct deevee_core
{
   bool initialized;
   bool loaded;
   bool menu_playback_active;
   uint64_t frame_count;
   struct deevee_content_info content;
   struct deevee_nav nav;
   struct deevee_video video;
   struct deevee_audio audio;
   struct deevee_dvdnav dvdnav;
   bool dvdnav_active;
   struct deevee_video_decoder decoder;
   uint32_t *decoder_output_pixels;
   struct deevee_decoded_video_frame frame_queue[
      DEEVEE_VIDEO_FRAME_QUEUE_CAPACITY];
   size_t frame_queue_head;
   size_t frame_queue_count;
   int64_t frame_clock_pts;
   bool frame_clock_has_pts;
   unsigned display_frame_ticks_remaining;
   uint64_t displayed_frames;
   uint64_t repeated_frames;
   uint64_t decode_underruns;
   uint64_t queue_drops;
   uint64_t fallback_timing_frames;
   uint8_t *menu_video_payloads;
   size_t menu_video_payload_size;
   size_t menu_video_payload_capacity;
   struct deevee_video_payload_chunk *menu_video_chunks;
   size_t menu_video_chunk_count;
   size_t menu_video_chunk_capacity;
   size_t next_menu_video_chunk;
   uint8_t *audio_payloads;
   size_t audio_payload_size;
   size_t audio_payload_capacity;
   struct deevee_audio_payload_chunk *audio_chunks;
   size_t audio_chunk_count;
   size_t audio_chunk_capacity;
   size_t next_audio_chunk;
   uint64_t audio_payload_packets;
   unsigned menu_frame_hold;
   unsigned menu_frame_repeat;
   uint8_t video_frame_rate_code;
   unsigned video_frame_duration_ticks;
   char menu_playback_source[32];
   uint64_t menu_packets_sent;
   uint64_t menu_frames_decoded;
   unsigned menu_last_frame_width;
   unsigned menu_last_frame_height;
   int menu_last_pixel_format;
   uint8_t menu_button_count;
   uint8_t menu_active_button;
   uint8_t menu_confirmed_button;
   uint32_t menu_last_nav_mask;
   struct deevee_dvd_menu_button menu_buttons[DEEVEE_DVD_MAX_MENU_BUTTONS];
   uint8_t menu_post_command_count;
   uint8_t menu_post_commands[DEEVEE_DVD_MAX_PROBED_COMMANDS][8];
   uint8_t menu_resolved_jump_command[8];
   bool menu_has_resolved_jump;
   uint8_t menu_current_vts;
   enum deevee_dvd_menu_domain menu_domain;
   char menu_command_status[96];
   bool menu_at_end;
   bool menu_loop_enabled;
   bool playback_is_title;
   bool menu_last_frame_has_pts;
   int64_t menu_last_frame_pts;
   bool menu_previous_frame_has_pts;
   int64_t menu_previous_frame_pts;
};

bool deevee_core_init(struct deevee_core *core);
void deevee_core_deinit(struct deevee_core *core);
bool deevee_core_load(struct deevee_core *core, const char *path);
void deevee_core_unload(struct deevee_core *core);
void deevee_core_reset(struct deevee_core *core);
void deevee_core_set_button(struct deevee_core *core,
      enum deevee_nav_button button, bool pressed);
void deevee_core_run(struct deevee_core *core, struct deevee_frame *video,
      struct deevee_audio_frame *audio);
const char *deevee_core_loaded_content_type(const struct deevee_core *core);
bool deevee_core_menu_playback_active(const struct deevee_core *core);
const char *deevee_core_menu_playback_source(const struct deevee_core *core);
size_t deevee_core_menu_payload_count(const struct deevee_core *core);
size_t deevee_core_menu_payload_bytes(const struct deevee_core *core);
uint64_t deevee_core_menu_packets_sent(const struct deevee_core *core);
uint64_t deevee_core_menu_frames_decoded(const struct deevee_core *core);
uint64_t deevee_core_displayed_frames(const struct deevee_core *core);
uint64_t deevee_core_repeated_frames(const struct deevee_core *core);
uint64_t deevee_core_decode_underruns(const struct deevee_core *core);
uint64_t deevee_core_queued_frames(const struct deevee_core *core);
uint64_t deevee_core_queue_drops(const struct deevee_core *core);
uint64_t deevee_core_fallback_timing_frames(const struct deevee_core *core);
uint64_t deevee_core_audio_payload_count(const struct deevee_core *core);
uint64_t deevee_core_audio_packets_sent(const struct deevee_core *core);
uint64_t deevee_core_audio_decoded_frames(const struct deevee_core *core);
uint64_t deevee_core_audio_buffered_frames(const struct deevee_core *core);
uint64_t deevee_core_audio_decode_errors(const struct deevee_core *core);
uint64_t deevee_core_audio_underruns(const struct deevee_core *core);
unsigned deevee_core_audio_last_sample_rate(const struct deevee_core *core);
unsigned deevee_core_audio_last_channels(const struct deevee_core *core);
unsigned deevee_core_video_frame_rate_code(const struct deevee_core *core);
unsigned deevee_core_video_frame_duration_ticks(const struct deevee_core *core);
unsigned deevee_core_menu_last_frame_width(const struct deevee_core *core);
unsigned deevee_core_menu_last_frame_height(const struct deevee_core *core);
int deevee_core_menu_last_pixel_format(const struct deevee_core *core);
uint8_t deevee_core_menu_button_count(const struct deevee_core *core);
uint8_t deevee_core_menu_active_button(const struct deevee_core *core);
uint8_t deevee_core_menu_confirmed_button(const struct deevee_core *core);
uint8_t deevee_core_menu_confirmed_command_byte(
      const struct deevee_core *core, unsigned index);
bool deevee_core_menu_has_resolved_jump(const struct deevee_core *core);
uint8_t deevee_core_menu_resolved_jump_command_byte(
      const struct deevee_core *core, unsigned index);
const char *deevee_core_menu_command_status(const struct deevee_core *core);

#endif
