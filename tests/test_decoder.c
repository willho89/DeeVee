#include "deevee_decoder.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
   int ok = 1;
   struct deevee_video_decoder decoder;
   struct deevee_decoder_frame_probe frame_probe;
   enum deevee_decoder_status status;
   uint8_t payload = 0;

   memset(&frame_probe, 0, sizeof(frame_probe));

   status = deevee_decoder_init(&decoder);
   ok = ok && status == DEEVEE_DECODER_OK;
   ok = ok && decoder.initialized;
   ok = ok && strcmp(deevee_decoder_backend_name(),
         decoder.backend_name) == 0;

   status = deevee_decoder_open_mpeg2(&decoder);
#if HAVE_FFMPEG
   ok = ok && status == DEEVEE_DECODER_OK;
   ok = ok && deevee_decoder_is_available();
   ok = ok && decoder.opened;
   ok = ok && decoder.context != NULL;
   status = deevee_decoder_decode_mpeg2_payload(&decoder, NULL, 0,
         &frame_probe);
   ok = ok && status == DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;
   status = deevee_decoder_decode_mpeg2_payload(&decoder, &payload, 0,
         &frame_probe);
   ok = ok && status == DEEVEE_DECODER_ERROR_INVALID_ARGUMENT;
#else
   ok = ok && status == DEEVEE_DECODER_ERROR_UNAVAILABLE;
   ok = ok && !deevee_decoder_is_available();
   ok = ok && !decoder.opened;
   ok = ok && decoder.context == NULL;
   status = deevee_decoder_decode_mpeg2_payload(&decoder, &payload, 1,
         &frame_probe);
   ok = ok && status == DEEVEE_DECODER_ERROR_UNAVAILABLE;
#endif

   if (!ok)
      fprintf(stderr, "decoder test failed with status %s\n",
            deevee_decoder_status_name(status));

   deevee_decoder_deinit(&decoder);
   return ok ? 0 : 1;
}
