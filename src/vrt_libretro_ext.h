/* vrt_libretro_ext.h - VRT frontend extensions to the libretro API.
 *
 * Canonical copy: VRT/include/vrt_libretro_ext.h. Cores and the VRT host fork
 * of libretro-godot carry verbatim copies; keep them in sync.
 *
 * Public domain / CC0: copy freely into any core.
 *
 * Every command lives in libretro's frontend-private range, so other frontends
 * (RetroArch etc.) return false and a core simply keeps its normal behaviour.
 */
#ifndef VRT_LIBRETRO_EXT_H__
#define VRT_LIBRETRO_EXT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Value of RETRO_ENVIRONMENT_PRIVATE in libretro.h, repeated so this header
 * does not depend on it. 0x5652 is "VR". */
#define VRT_ENVIRONMENT_BASE (0x20000 | 0x5652 << 4)

/* ---------------------------------------------------------------------------
 * VRT_ENVIRONMENT_SET_VIDEO_FIELD  (const struct vrt_video_field *)
 *
 * Interlaced video for a CRT that draws fields the way a real one does.
 *
 * Probe: call with data == NULL. Returns true if the frontend supports field
 * output; only then should a core switch into field mode.
 *
 * Set: describes every frame passed to video_refresh from now on, until set
 * again ("sticky"). In field mode each video_refresh carries ONE field:
 *   - the buffer holds only that field's lines (e.g. 720x240 for 480i),
 *   - VRT_FIELD_TOP lines land on even raster lines (0, 2, 4, ...) of the
 *     full frame, VRT_FIELD_BOTTOM on odd ones,
 *   - video_refresh runs at field rate; the core's declared fps
 *     (retro_system_timing) must be the field rate too (59.94 for NTSC).
 * Set VRT_FIELD_PROGRESSIVE to return to ordinary whole frames.
 *
 * Hosts without the extension never see field-mode frames, because the core
 * only enters field mode after a successful probe.
 * ------------------------------------------------------------------------- */
#define VRT_ENVIRONMENT_SET_VIDEO_FIELD (VRT_ENVIRONMENT_BASE + 1)

enum vrt_field
{
   VRT_FIELD_PROGRESSIVE = 0,
   VRT_FIELD_TOP         = 1,
   VRT_FIELD_BOTTOM      = 2
};

struct vrt_video_field
{
   unsigned field;       /* enum vrt_field */
   unsigned frame_lines; /* lines in the full (woven) frame, e.g. 480 or 576;
                            0 for progressive */
};

#ifdef __cplusplus
}
#endif

#endif
