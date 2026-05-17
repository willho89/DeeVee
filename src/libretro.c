#include "libretro.h"

#include "deevee_core.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEEVEE_CORE_VERSION "0.1.0-dev"

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_log_callback logging;
static struct deevee_core core;

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

static void deevee_update_input(void)
{
   if (input_poll_cb)
      input_poll_cb();

   deevee_core_set_button(&core, DEEVEE_NAV_UP,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_UP));
   deevee_core_set_button(&core, DEEVEE_NAV_DOWN,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_DOWN));
   deevee_core_set_button(&core, DEEVEE_NAV_LEFT,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_LEFT));
   deevee_core_set_button(&core, DEEVEE_NAV_RIGHT,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_RIGHT));
   deevee_core_set_button(&core, DEEVEE_NAV_CONFIRM,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_A));
   deevee_core_set_button(&core, DEEVEE_NAV_CANCEL,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_B));
   deevee_core_set_button(&core, DEEVEE_NAV_MENU,
         deevee_button(RETRO_DEVICE_ID_JOYPAD_START) ||
         deevee_button(RETRO_DEVICE_ID_JOYPAD_X));
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
   info->valid_extensions = "iso|chd|ifo";
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
   deevee_core_run(&core, &video, &audio);

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
