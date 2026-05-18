#include "libretro.h"

#include "deevee_core.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEEVEE_CORE_VERSION "0.5.0-alpha"

#ifndef RETRO_ENVIRONMENT_GET_VARIABLE
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_SET_VARIABLES 16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
struct retro_variable
{
   const char *key;
   const char *value;
};
#endif

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_log_callback logging;
static struct deevee_core core;
static unsigned diagnostic_frame_counter;
static uint8_t logged_confirmed_button;
static char current_audio_option[DEEVEE_TRACK_LABEL_LENGTH] = "DVD default";
static char current_subtitle_option[DEEVEE_TRACK_LABEL_LENGTH] = "DVD default";
static char audio_option_values[512] =
   "Audio Track; DVD default|Track 1|Track 2|Track 3|Track 4|"
   "Track 5|Track 6|Track 7|Track 8";
static char subtitle_option_values[1536] =
   "Subtitle Track; DVD default|Off|Track 1|Track 2|Track 3|Track 4|"
   "Track 5|Track 6|Track 7|Track 8|Track 9|Track 10|Track 11|Track 12|"
   "Track 13|Track 14|Track 15|Track 16|Track 17|Track 18|Track 19|"
   "Track 20|Track 21|Track 22|Track 23|Track 24|Track 25|Track 26|"
   "Track 27|Track 28|Track 29|Track 30|Track 31|Track 32";

static struct retro_variable deevee_core_variables[] = {
   { "deevee_audio_track", audio_option_values },
   { "deevee_subtitle_track", subtitle_option_values },
   { NULL, NULL }
};

static const struct retro_input_descriptor deevee_input_descriptors[] = {
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,
      "Navigate Up" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,
      "Navigate Down" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,
      "Navigate Left" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,
      "Navigate Right" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Confirm" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Back" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Home" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,
      "Play/Pause" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,
      "Stop" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,
      "Previous Chapter" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,
      "Next Chapter" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Rewind" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,
      "Fast-Forward" },
   { 0, 0, 0, 0, NULL }
};

static void deevee_log(enum retro_log_level level, const char *fmt, ...)
{
   char message[1024];
   va_list args;

   if (!logging.log)
      return;

   va_start(args, fmt);
   vsnprintf(message, sizeof(message), fmt, args);
   va_end(args);
   logging.log(level, "%s", message);
}

static bool deevee_button(unsigned id)
{
   if (!input_state_cb)
      return false;

   return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

static bool deevee_key(unsigned id)
{
   if (!input_state_cb)
      return false;

   return input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, id) != 0;
}

static void deevee_update_input(void)
{
   if (input_poll_cb)
      input_poll_cb();

   deevee_core_set_button(&core, DEEVEE_NAV_UP,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_UP) ||
         deevee_key(RETROK_UP));
   deevee_core_set_button(&core, DEEVEE_NAV_DOWN,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_DOWN) ||
         deevee_key(RETROK_DOWN));
   deevee_core_set_button(&core, DEEVEE_NAV_LEFT,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_LEFT) ||
         deevee_key(RETROK_LEFT));
   deevee_core_set_button(&core, DEEVEE_NAV_RIGHT,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_RIGHT) ||
         deevee_key(RETROK_RIGHT));
   deevee_core_set_button(&core, DEEVEE_NAV_CONFIRM,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_B) ||
         deevee_key(RETROK_RETURN));
   deevee_core_set_button(&core, DEEVEE_NAV_CANCEL,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_A) ||
         deevee_key(RETROK_BACKSPACE) ||
         deevee_key(RETROK_ESCAPE));
   deevee_core_set_button(&core, DEEVEE_NAV_HOME,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_X) ||
         deevee_key(RETROK_HOME));
   deevee_core_set_button(&core, DEEVEE_NAV_PLAY_PAUSE,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_START) ||
         deevee_key(RETROK_SPACE));
   deevee_core_set_button(&core, DEEVEE_NAV_STOP,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_SELECT));
   deevee_core_set_button(&core, DEEVEE_NAV_PREVIOUS_CHAPTER,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_L) ||
         deevee_key(RETROK_PAGEUP));
   deevee_core_set_button(&core, DEEVEE_NAV_NEXT_CHAPTER,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_R) ||
         deevee_key(RETROK_PAGEDOWN));
   deevee_core_set_button(&core, DEEVEE_NAV_REWIND,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_L2) ||
         deevee_key(RETROK_COMMA));
   deevee_core_set_button(&core, DEEVEE_NAV_FAST_FORWARD,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_R2) ||
         deevee_key(RETROK_PERIOD));
}

static int track_index_from_value(const char *value)
{
   int index = 0;

   if (!value || strncmp(value, "Track ", 6) != 0)
      return -1;

   value += 6;
   while (*value >= '0' && *value <= '9')
   {
      index = index * 10 + (*value - '0');
      value++;
   }

   return index > 0 ? index - 1 : -1;
}

static void append_option_value(char *buffer, size_t buffer_size,
      const char *value)
{
   size_t used;

   if (!buffer || !buffer_size || !value || !value[0])
      return;

   used = strlen(buffer);
   if (used >= buffer_size)
      return;
   snprintf(buffer + used, buffer_size - used, "|%s", value);
}

static void deevee_refresh_core_option_labels(void)
{
   unsigned i;
   unsigned count;
   char fallback[16];

   snprintf(audio_option_values, sizeof(audio_option_values),
         "Audio Track; DVD default");
   count = deevee_core_audio_stream_count(&core);
   if (count)
   {
      for (i = 0; i < count; i++)
         append_option_value(audio_option_values, sizeof(audio_option_values),
               deevee_core_audio_stream_label(&core, i));
   }
   else
   {
      for (i = 0; i < DEEVEE_MAX_AUDIO_STREAMS; i++)
      {
         snprintf(fallback, sizeof(fallback), "Track %u", i + 1u);
         append_option_value(audio_option_values, sizeof(audio_option_values),
               fallback);
      }
   }

   snprintf(subtitle_option_values, sizeof(subtitle_option_values),
         "Subtitle Track; DVD default|Off");
   count = deevee_core_subpicture_stream_count(&core);
   if (count)
   {
      for (i = 0; i < count; i++)
         append_option_value(subtitle_option_values,
               sizeof(subtitle_option_values),
               deevee_core_subpicture_stream_label(&core, i));
   }
   else
   {
      for (i = 0; i < DEEVEE_MAX_SUBTITLE_STREAMS; i++)
      {
         snprintf(fallback, sizeof(fallback), "Track %u", i + 1u);
         append_option_value(subtitle_option_values,
               sizeof(subtitle_option_values), fallback);
      }
   }

   if (environ_cb)
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES,
            (void *)deevee_core_variables);
   deevee_core_clear_track_options_dirty(&core);
}

static void deevee_apply_core_options(void)
{
   struct retro_variable variable;

   if (!environ_cb)
      return;

   memset(&variable, 0, sizeof(variable));
   variable.key = "deevee_audio_track";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) &&
         variable.value)
   {
      bool changed = strcmp(variable.value, current_audio_option) != 0;
      bool applied = true;

      if (strcmp(variable.value, "DVD default") == 0)
      {
         if (changed)
            applied = deevee_core_set_audio_track(&core, -1);
      }
      else if (changed)
         applied = deevee_core_set_audio_track(&core,
               track_index_from_value(variable.value));
      if (applied)
         snprintf(current_audio_option, sizeof(current_audio_option), "%s",
               variable.value);
   }

   memset(&variable, 0, sizeof(variable));
   variable.key = "deevee_subtitle_track";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) &&
         variable.value)
   {
      bool changed = strcmp(variable.value, current_subtitle_option) != 0;
      bool applied = true;

      if (strcmp(variable.value, "DVD default") == 0)
      {
         if (changed)
            applied = deevee_core_set_subtitle_track(&core, -1, true);
      }
      else if (strcmp(variable.value, "Off") == 0)
      {
         if (changed)
            applied = deevee_core_set_subtitle_track(&core, -1, false);
      }
      else if (changed)
         applied = deevee_core_set_subtitle_track(&core,
               track_index_from_value(variable.value), true);
      if (applied)
         snprintf(current_subtitle_option, sizeof(current_subtitle_option), "%s",
               variable.value);
   }
}

static void deevee_check_core_options(void)
{
   bool updated = false;

   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) &&
         updated)
      deevee_apply_core_options();
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
   if (!info)
      return;

   memset(info, 0, sizeof(*info));
   info->library_name = "DeeVee";
   info->library_version = DEEVEE_CORE_VERSION;
   info->valid_extensions = "iso|chd";
   info->need_fullpath = true;
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   if (!info)
      return;

   memset(info, 0, sizeof(*info));
   info->geometry.base_width = DEEVEE_VIDEO_WIDTH;
   info->geometry.base_height = DEEVEE_VIDEO_HEIGHT;
   info->geometry.max_width = DEEVEE_VIDEO_WIDTH;
   info->geometry.max_height = DEEVEE_VIDEO_HEIGHT;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps = DEEVEE_VIDEO_FPS;
   info->timing.sample_rate = DEEVEE_AUDIO_SAMPLE_RATE;
}

void retro_init(void)
{
   enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
   bool supports_no_game = false;

   if (environ_cb)
   {
      environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging);
      environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_format);
      environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &supports_no_game);
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
            (void *)deevee_input_descriptors);
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES,
            (void *)deevee_core_variables);
   }

   if (!deevee_core_init(&core))
      deevee_log(RETRO_LOG_ERROR, "DeeVee: failed to initialize core state.\n");
}

void retro_deinit(void)
{
   deevee_core_deinit(&core);
   memset(&logging, 0, sizeof(logging));
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_reset(void)
{
   deevee_core_reset(&core);
}

bool retro_load_game(const struct retro_game_info *game)
{
   if (!game || !game->path)
   {
      deevee_log(RETRO_LOG_ERROR, "DeeVee: missing content path.\n");
      return false;
   }

   if (!deevee_core_load(&core, game->path))
   {
      deevee_log(RETRO_LOG_ERROR,
            "DeeVee: unsupported or missing DVD content: %s\n", game->path);
      return false;
   }

   deevee_log(RETRO_LOG_INFO, "DeeVee: loaded %s.\n",
         deevee_core_loaded_content_type(&core));
   if (deevee_core_track_options_dirty(&core))
      deevee_refresh_core_option_labels();
   deevee_apply_core_options();
   deevee_log(RETRO_LOG_INFO,
         "DeeVee: menu playback %s, source=%s, payloads=%u, bytes=%u, "
         "audio_payloads=%llu, decoded_frames=%llu, queued=%llu, "
         "spu_payloads=%llu spu_ready=%u spu_rects=%u spu_stream=0x%02x "
         "audio_stream=0x%02x audio_tracks=%u audio_active=%d/%d "
         "subtitle_tracks=%u subtitle_active=%d/%d subtitle_visible=%u "
         "rate_code=%u frame_ticks=%u, "
         "frame=%ux%u, pixfmt=%d, "
         "buttons=%u, active=%u, command_status=%s.\n",
         deevee_core_menu_playback_active(&core) ? "active" : "inactive",
         deevee_core_menu_playback_source(&core),
         (unsigned)deevee_core_menu_payload_count(&core),
         (unsigned)deevee_core_menu_payload_bytes(&core),
         (unsigned long long)deevee_core_audio_payload_count(&core),
         (unsigned long long)deevee_core_menu_frames_decoded(&core),
         (unsigned long long)deevee_core_queued_frames(&core),
         (unsigned long long)deevee_core_subpicture_payload_count(&core),
         deevee_core_subpicture_frame_ready(&core) ? 1u : 0u,
         deevee_core_subpicture_rect_count(&core),
         deevee_core_active_subpicture_stream(&core),
         deevee_core_active_audio_stream(&core),
         deevee_core_audio_stream_count(&core),
         deevee_core_active_audio_logical_stream(&core),
         deevee_core_active_audio_physical_stream(&core),
         deevee_core_subpicture_stream_count(&core),
         deevee_core_active_subpicture_logical_stream(&core),
         deevee_core_active_subpicture_physical_stream(&core),
         deevee_core_subpicture_visible(&core) ? 1u : 0u,
         deevee_core_video_frame_rate_code(&core),
         deevee_core_video_frame_duration_ticks(&core),
         deevee_core_menu_last_frame_width(&core),
         deevee_core_menu_last_frame_height(&core),
         deevee_core_menu_last_pixel_format(&core),
         (unsigned)deevee_core_menu_button_count(&core),
         (unsigned)deevee_core_menu_active_button(&core),
         deevee_core_menu_command_status(&core));
   diagnostic_frame_counter = 0;
   return true;
}

bool retro_load_game_special(unsigned game_type,
      const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

void retro_unload_game(void)
{
   deevee_core_unload(&core);
   diagnostic_frame_counter = 0;
   logged_confirmed_button = 0;
   snprintf(current_audio_option, sizeof(current_audio_option), "DVD default");
   snprintf(current_subtitle_option, sizeof(current_subtitle_option),
         "DVD default");
}

unsigned retro_get_region(void)
{
   return 0;
}

void retro_run(void)
{
   struct deevee_frame video;
   struct deevee_audio_frame audio;

   memset(&video, 0, sizeof(video));
   memset(&audio, 0, sizeof(audio));

   deevee_update_input();
   deevee_check_core_options();
   deevee_core_run(&core, &video, &audio);
   if (deevee_core_track_options_dirty(&core))
      deevee_refresh_core_option_labels();
   diagnostic_frame_counter++;

   if (deevee_core_menu_confirmed_button(&core) &&
         deevee_core_menu_confirmed_button(&core) != logged_confirmed_button)
   {
      logged_confirmed_button = deevee_core_menu_confirmed_button(&core);
      deevee_log(RETRO_LOG_INFO,
            "DeeVee: menu button %u confirmed, command="
            "%02x%02x%02x%02x%02x%02x%02x%02x.\n",
            (unsigned)logged_confirmed_button,
            deevee_core_menu_confirmed_command_byte(&core, 0),
            deevee_core_menu_confirmed_command_byte(&core, 1),
            deevee_core_menu_confirmed_command_byte(&core, 2),
            deevee_core_menu_confirmed_command_byte(&core, 3),
            deevee_core_menu_confirmed_command_byte(&core, 4),
            deevee_core_menu_confirmed_command_byte(&core, 5),
            deevee_core_menu_confirmed_command_byte(&core, 6),
            deevee_core_menu_confirmed_command_byte(&core, 7));
      if (deevee_core_menu_has_resolved_jump(&core))
         deevee_log(RETRO_LOG_INFO,
               "DeeVee: resolved menu jump command="
               "%02x%02x%02x%02x%02x%02x%02x%02x.\n",
               deevee_core_menu_resolved_jump_command_byte(&core, 0),
               deevee_core_menu_resolved_jump_command_byte(&core, 1),
               deevee_core_menu_resolved_jump_command_byte(&core, 2),
               deevee_core_menu_resolved_jump_command_byte(&core, 3),
               deevee_core_menu_resolved_jump_command_byte(&core, 4),
               deevee_core_menu_resolved_jump_command_byte(&core, 5),
               deevee_core_menu_resolved_jump_command_byte(&core, 6),
               deevee_core_menu_resolved_jump_command_byte(&core, 7));
   }

   if (diagnostic_frame_counter == 1 ||
         diagnostic_frame_counter % 120u == 0u)
      deevee_log(RETRO_LOG_INFO,
            "DeeVee: run menu=%s source=%s packets=%llu frames=%llu "
            "displayed=%llu queued=%llu repeated=%llu underruns=%llu "
            "drops=%llu fallback=%llu rate_code=%u frame_ticks=%u "
            "last_frame_ticks=%u repeat_pict=%d "
            "dvdnav=%s pos=%s title=%d part=%d/%d time=%lld "
            "frame=%ux%u pixfmt=%d buttons=%u "
            "active=%u confirmed=%u command_status=%s "
            "audio_payloads=%llu audio_packets=%llu "
            "audio_frames=%llu audio_buffer=%llu audio_errors=%llu "
            "audio_underruns=%llu audio_rate=%u audio_channels=%u "
            "audio_stream=0x%02x "
            "audio_tracks=%u audio_active=%d/%d "
            "subtitle_tracks=%u subtitle_active=%d/%d subtitle_visible=%u "
            "spu_payloads=%llu spu_ready=%u spu_rects=%u "
            "spu_stream=0x%02x spu_errors=%llu.\n",
            deevee_core_menu_playback_active(&core) ? "active" : "inactive",
            deevee_core_menu_playback_source(&core),
            (unsigned long long)deevee_core_menu_packets_sent(&core),
            (unsigned long long)deevee_core_menu_frames_decoded(&core),
            (unsigned long long)deevee_core_displayed_frames(&core),
            (unsigned long long)deevee_core_queued_frames(&core),
            (unsigned long long)deevee_core_repeated_frames(&core),
            (unsigned long long)deevee_core_decode_underruns(&core),
            (unsigned long long)deevee_core_queue_drops(&core),
            (unsigned long long)deevee_core_fallback_timing_frames(&core),
            deevee_core_video_frame_rate_code(&core),
            deevee_core_video_frame_duration_ticks(&core),
            deevee_core_last_decoded_frame_duration_ticks(&core),
            deevee_core_last_decoded_repeat_pict(&core),
            deevee_core_dvdnav_active(&core) ? "active" : "inactive",
            deevee_core_dvdnav_has_position(&core) ? "valid" : "unknown",
            deevee_core_dvdnav_title(&core),
            deevee_core_dvdnav_part(&core),
            deevee_core_dvdnav_parts(&core),
            (long long)deevee_core_dvdnav_time_ticks(&core),
            deevee_core_menu_last_frame_width(&core),
            deevee_core_menu_last_frame_height(&core),
            deevee_core_menu_last_pixel_format(&core),
            (unsigned)deevee_core_menu_button_count(&core),
            (unsigned)deevee_core_menu_active_button(&core),
            (unsigned)deevee_core_menu_confirmed_button(&core),
            deevee_core_menu_command_status(&core),
            (unsigned long long)deevee_core_audio_payload_count(&core),
            (unsigned long long)deevee_core_audio_packets_sent(&core),
            (unsigned long long)deevee_core_audio_decoded_frames(&core),
            (unsigned long long)deevee_core_audio_buffered_frames(&core),
            (unsigned long long)deevee_core_audio_decode_errors(&core),
            (unsigned long long)deevee_core_audio_underruns(&core),
            deevee_core_audio_last_sample_rate(&core),
            deevee_core_audio_last_channels(&core),
            deevee_core_active_audio_stream(&core),
            deevee_core_audio_stream_count(&core),
            deevee_core_active_audio_logical_stream(&core),
            deevee_core_active_audio_physical_stream(&core),
            deevee_core_subpicture_stream_count(&core),
            deevee_core_active_subpicture_logical_stream(&core),
            deevee_core_active_subpicture_physical_stream(&core),
            deevee_core_subpicture_visible(&core) ? 1u : 0u,
            (unsigned long long)deevee_core_subpicture_payload_count(&core),
            deevee_core_subpicture_frame_ready(&core) ? 1u : 0u,
            deevee_core_subpicture_rect_count(&core),
            deevee_core_active_subpicture_stream(&core),
            (unsigned long long)deevee_core_subpicture_decode_errors(&core));

   if (video_cb)
      video_cb(video.pixels, video.width, video.height, video.pitch);

   if (audio_batch_cb && audio.samples)
      audio_batch_cb(audio.samples, audio.frames);
   else if (audio_cb)
      audio_cb(0, 0);
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}
