/*
 * DeeVee — libretro DVD-Video core
 * Copyright (C) 2026 DeeVee contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 2048-byte PS pack -> PES -> elementary stream router (CLAUDE.md §5.3).
 */
#ifndef DV_PS_H
#define DV_PS_H

#include <stddef.h>
#include <stdint.h>

struct dv_ctx;

/* Stream IDs as VLC numbers them: plain PES ids are 0x00xx, private stream 1
 * substreams are 0xBDxx. */
#define DV_PS_ID_VIDEO       0x00E0
#define DV_PS_IS_MPA(id)     (((id) & 0xFFF8) == 0x00C0)
#define DV_PS_IS_SPU(id)     (((id) & 0xFFE0) == 0xBD20)
#define DV_PS_IS_AC3(id)     (((id) & 0xFFF8) == 0xBD80)
#define DV_PS_IS_DTS(id)     (((id) & 0xFFF8) == 0xBD88)
#define DV_PS_IS_LPCM(id)    (((id) & 0xFFF8) == 0xBDA0)

typedef struct dv_ps_stats
{
   uint32_t packs;
   uint32_t video_pes, audio_pes, spu_pes, other_pes, errors;
   uint32_t scrambled;   /* CSS-scrambled PES not decrypted (dropped) */
} dv_ps_stats_t;

/* Demux one pack (or any buffer of whole PS packets) and route payloads. */
void dv_ps_demux(struct dv_ctx *ctx, const uint8_t *buf, size_t len);

#endif
