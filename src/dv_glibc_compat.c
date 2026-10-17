/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linux ARM builds only (platform=linux-arm64 / linux-armhf): glibc 2.38
 * gave fmod a new symbol version on ARM, which alone would stop the core
 * loading on glibc 2.36 (Raspberry Pi OS / Debian 12). Route every fmod
 * reference (-Wl,--wrap=fmod, FFmpeg included) to the original version.
 */
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))

#if defined(__aarch64__)
__asm__(".symver dv_fmod_base,fmod@GLIBC_2.17");
#else
__asm__(".symver dv_fmod_base,fmod@GLIBC_2.4");
#endif

double dv_fmod_base(double x, double y);
double __wrap_fmod(double x, double y);

double __wrap_fmod(double x, double y)
{
   return dv_fmod_base(x, y);
}

#endif
