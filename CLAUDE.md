# DeeVee — libretro DVD-Video Core: Master Development Plan
 
**Status:** v1.0 (2026-10-15) — authoritative guide for all development sessions
**Goal:** A libretro core that plays DVD-Video (ISO, VIDEO_TS folder, physical disc) in RetroArch, region-free, with full interactive menu navigation and correct SPU (subpicture) rendering, including button highlights.
 
---
 
## 0. How to use this document (session protocol)
 
Every development session should:
 
1. Read this file first, then the **Status Board** (§12) and **Decision Log** (§13).
2. Pick the next open milestone task; don't skip ahead of a milestone's acceptance criteria.
3. Before writing a subsystem, open the **reference source** listed for it (§4) and port/adapt rather than invent.
4. End the session by updating the Status Board, adding any decisions to the Decision Log, and noting open questions.
Guiding rule: **don't reinvent the wheel.** The DVD virtual machine, IFO parsing, CSS and codecs already exist in mature libraries. DeeVee's own code is the *glue*: libretro plumbing, a PS/PES packet router, the SPU + highlight compositor, the A/V clock, and input mapping. Wherever glue logic has a precedent in VLC (or mpv/FFmpeg), port it.
 
---
 
## 1. Scope
 
### In scope (v1)
- Content: `.iso` images, `.chd` images (MAME `chdman createdvd` DVD CHDs), `VIDEO_TS` folders (load `VIDEO_TS.IFO`), mounted physical discs (load `VIDEO_TS.IFO` from the mount; libdvdread resolves the block device for CSS).
- Full DVD VM: First Play PGC, root/title/chapter/audio/subtitle/angle menus, button navigation and activation, still frames, seamless branching, multi-angle, parental level, RCE discs.
- Video: MPEG-1/MPEG-2, NTSC (720×480) and PAL (720×576) incl. 704/352 widths, 4:3 and anamorphic 16:9, letterbox/pan-scan flags, 3:2 pulldown (RFF/TFF), interlaced content.
- Audio: AC-3, DTS, LPCM (16/20/24-bit, 48/96 kHz), MPEG-1 Layer II. Output fixed 48 kHz stereo (downmix).
- Subpictures: subtitles (incl. forced), menu overlays, button highlight palettes, CLUT changes.
- Region: any region disc plays; VM region auto-set from disc mask to defeat RCE.
- Input: RetroPad, mouse, and pointer/touch.
### Out of scope (v1, revisit later)
- DVD-Audio, DVD-VR (+VR/-VR recordings), Blu-ray.
- Closed captions (Line 21) — planned for a later milestone.
- Bit-exact passthrough (S/PDIF/HDMI bitstream) — not possible through libretro audio.
- Full deterministic save states (we'll do "resume points" first).
---
 
## 2. Licensing (decide once, early)
 
| Component | License | Notes |
|---|---|---|
| libdvdnav, libdvdread | GPL-2.0-or-later | Required. |
| libdvdcss | GPL-2.0-or-later | Needed for CSS-encrypted discs/ISOs. Distribution sensitivity — see D-003. |
| FFmpeg (libavcodec/libswscale/libswresample) | LGPL-2.1+ (GPL if GPL parts enabled) | Build only decoders we need. |
| VLC modules we port from | GPL-2.0+/LGPL-2.1+ | Keep original copyright headers on ported files. |
| libretro-common | MIT | |
| libchdr | BSD-3-Clause | CHD reading (M5). Used by many libretro cores. |
 
**DeeVee license: GPL-2.0-or-later.** This is compatible with everything above, lets us lift VLC code directly, and keeps RetroArch (GPLv3) distribution clean. Ported files keep the upstream copyright notice plus "Adapted for DeeVee" lines.
 
---
 
## 3. Architecture overview
 
```
 RetroArch (frontend)
   │ retro_load_game / retro_run / input_state / video_refresh / audio_batch
   ▼
┌──────────────────────────── DeeVee core ─────────────────────────────┐
│ libretro.c        entry points, av_info, core options, OSD messages  │
│ dv_source.c       content resolution + libretro VFS stream callbacks │
│ dv_nav.c          libdvdnav loop & event dispatcher  ◄─ VLC dvdnav.c │
│ dv_ps.c           2048-byte PS pack → PES → elementary-stream router │
│                                         ◄─ VLC demux/mpeg/ps.h,pes.h │
│ dv_video.c        libavcodec mpeg2video → frame queue, RFF handling  │
│ dv_audio.c        libavcodec ac3/dca/pcm_dvd/mp2 → swresample 48k   │
│ dv_spu.c          SPU parser (RLE + DCSQ) ◄─ VLC codec/spudec/parse.c│
│                                           + FFmpeg dvdsubdec.c       │
│ dv_overlay.c      CLUT, highlight rect/palette, blend onto frame     │
│ dv_clock.c        master timeline, VOBU discontinuities, still/wait  │
│ dv_input.c        RetroPad/mouse/pointer → dvdnav button/menu calls  │
│ dv_render.c       yuv420p → XRGB8888 (swscale) [later: GL path]      │
└──────────────────────────────────────────────────────────────────────┘
   │                    │                       │
 libdvdnav ── libdvdread ── libdvdcss       FFmpeg (decoders only)
```
 
### Key architectural decisions (rationale in §13)
- **libdvdnav owns navigation.** We never re-implement the VM. Its main loop is "get next block, handle event" — exactly what VLC and libdvdnav's own `examples/menus.c` do.
- **We do our own PS/PES demux, not FFmpeg's.** libdvdnav hands us 2 KB packs interleaved with navigation events (cell changes, highlight, still, wait, stream changes). Feeding that through an `AVIOContext` into FFmpeg's `mpeg` demuxer loses the event timing. VLC's approach — parse packs directly and route by stream ID — is small (~400 lines) and precise.
- **We write our own SPU decoder, not FFmpeg's `dvdsubdec`.** Menus need the decoded image kept as **2-bit palette indices** so the highlight palette/alpha can be swapped inside the button rectangle on every button move without re-decoding. FFmpeg's decoder outputs a resolved palette bitmap. We borrow its RLE logic as a cross-check, but structure follows VLC's `spudec`.
- **FFmpeg is used for codecs only** (mpeg2video, ac3, dca, pcm_dvd, mp2) plus swscale/swresample, built with `--disable-everything` + explicit enables.
- **Pull model, single-threaded first.** SD MPEG-2 decode costs ~1–3 ms/frame on modern CPUs, so `retro_run` can pull blocks until it has what it needs for the current tick. This works on thread-less platforms and keeps state simple. A read-ahead thread is added later *only* for physical drives (seek latency).
---
 
## 4. Reference source map (read before writing each subsystem)
 
**VLC** (`code.videolan.org/videolan/vlc`; use the `3.0.x` branch for stable behaviour, `master` for newer refactors):
 
| DeeVee module | VLC reference | What to take |
|---|---|---|
| dv_nav.c | `modules/access/dvdnav.c` | Open/config sequence, `DemuxBlock()` event switch, `ButtonUpdate()` (highlight palette math), language selection, region handling, still/wait handling, mouse → button mapping, title/chapter/seek control. |
| dv_ps.c | `modules/demux/mpeg/ps.h`, `modules/demux/mpeg/pes.h` | `ps_pkt_id()` private-stream-1 sub-ID routing, PES header/PTS parsing, LPCM header skip, SCR handling. |
| dv_spu.c | `modules/codec/spudec/spudec.c`, `parse.c`, `spudec.h` | SPU reassembly across packets, control sequence (DCSQ) parsing, RLE decode, forced-display flag, start/stop delays. |
| dv_overlay.c | `src/video_output/vout_subpictures.c` (+ highlight handling via `ES_OUT_SPU_SET_HIGHLIGHT` in master) | Applying the menu highlight palette to the region, alpha scaling. |
| fallback (non-nav) | `modules/access/dvdread.c` | Title-only playback path, IFO querying for OSD. |
| dv_audio.c | `modules/codec/lpcm.c` | DVD LPCM header formats (20/24-bit packing). |
| Closed captions (later) | `modules/codec/cc.c`, `modules/packetizer/mpegvideo.c` | DVD Line-21 user-data extraction. |
 
**libdvdnav** (`code.videolan.org/videolan/libdvdnav`):
- `src/dvdnav/dvdnav.h` — the API contract; read fully.
- `examples/menus.c` — canonical minimal event loop. Start M1 from this.
**FFmpeg** (`git.ffmpeg.org/ffmpeg.git`):
 
| Use | File | Notes |
|---|---|---|
| DVD demux reference | `libavformat/dvdvideodec.c` | Modern libdvdnav/libdvdread consumer; good for PTS-discontinuity and cell-boundary handling, chapter/title enumeration, and how it uses the IFO for stream metadata. Its `menu` mode demuxes menu VOBs via dvdread but does **not** run interactive navigation — that's our job. |
| PS sub-ID routing | `libavformat/mpeg.c` | Cross-check stream-ID mapping and LPCM header bytes. |
| SPU RLE | `libavcodec/dvdsubdec.c` | Cross-check RLE decode and control parsing; palette/alpha handling. |
| LPCM | `libavcodec/pcm-dvd.c` | Decoder we'll actually use. |
| MPEG-2 | `libavcodec/mpeg12dec.c` | Decoder; RFF/TFF/progressive flags come out on `AVFrame`. |
| Downmix | `libswresample` | 5.1→stereo matrix, 96k→48k. |
 
**mpv** (`stream/stream_dvdnav.c` in older releases): secondary reference for highlight events and audio/SPU logical-stream ↔ language mapping. Note its maintainers found time-based seeking in libdvdnav unreliable; prefer VLC's approach (see M6).
 
**libretro**: `libretro-common/include/libretro.h` (environment calls), the existing **FFmpeg core** (`RetroArch/cores/libretro-ffmpeg/ffmpeg_core.c`) for prior art on A/V pacing inside libretro and what *not* to do (its threaded FIFO model adds latency that hurts menu responsiveness).
 
---
 
## 5. Subsystem specifications
 
### 5.1 Content loading (dv_source.c)
- `retro_system_info`: `valid_extensions = "iso|ifo|img|chd"`, `need_fullpath = true`, `block_extract = true`.
- Resolution rules:
  - `*.iso` / `*.img` → open via `dvdnav_open_stream2()` with read/seek callbacks backed by the libretro VFS interface (`RETRO_ENVIRONMENT_GET_VFS_INTERFACE`), with a plain-path fallback. This makes Android SAF and sandboxed platforms work.
  - `*.chd` → DVD CHD via **libchdr** (hunk reads behind the same `dvdnav_open_stream2()` stream callbacks; sector size 2048, `DVD ` metadata from `chdman createdvd`). Underlying file I/O goes through the libretro VFS like ISOs. CSS works unchanged (libdvdcss stream mode).
  - `VIDEO_TS.IFO` → pass the parent directory (or mount root) to `dvdnav_open2()`; libdvdread maps a mounted disc to its device so libdvdcss can authenticate.
  - Later: a tiny `.dvd` text file containing a raw device path (`/dev/sr0`, `D:`) for direct drive access without mounting.
- Use the `_open2`/`_open_stream2` variants to route libdvdnav/libdvdread logs into `retro_log_printf`.
- Set `DVDCSS_CACHE` to `<system_dir>/deevee/dvdcss` before opening (key cache); expose `DVDCSS_METHOD` as a core option.
### 5.2 Navigation loop (dv_nav.c)
Port VLC's `DemuxBlock()` switch. Use `dvdnav_get_next_cache_block()` / `dvdnav_free_cache_block()` with read-ahead on. Event handling:
 
| Event | Action |
|---|---|
| `BLOCK_OK` | Hand 2048-byte pack to `dv_ps`. |
| `NOP` | Ignore. |
| `STILL_FRAME` | Flush decoders (emit last picture), enter still state. Length 0xFF = infinite (wait for input); else start a timer and call `dvdnav_still_skip()` when it expires. Keep audio running if present. |
| `WAIT` | Drain queued video/audio to the presentation clock, then `dvdnav_wait_skip()`. |
| `SPU_CLUT_CHANGE` | Store the 16-entry YCrCb CLUT; rebuild RGBA palette cache. |
| `SPU_STREAM_CHANGE` / `AUDIO_STREAM_CHANGE` | Re-select active substream IDs (wide/letterbox/pan-scan SPU variants based on current aspect). |
| `VTS_CHANGE` | Re-read video attributes (aspect, NTSC/PAL, permitted display modes) → update geometry/timing via `SET_SYSTEM_AV_INFO` if changed; flush decoders. |
| `CELL_CHANGE` | Update title/chapter/time for OSD; mark potential discontinuity. |
| `NAV_PACKET` | Read PCI/DSI; track `vobu_s_ptm`/`vobu_e_ptm` for discontinuity detection (§5.6); if PCI has buttons, trigger highlight refresh. |
| `HIGHLIGHT` | Recompute highlight rect + palette (§5.5). |
| `HOP_CHANNEL` | Flush all decoders and queues (seek/jump happened). |
| `STOP` | End of disc → signal shutdown or return to root menu (core option). |
 
Startup config (mirror VLC's open sequence): `dvdnav_set_readahead_flag`, `dvdnav_set_PGC_positioning_flag(1)`, menu/audio/SPU language from core options, `dvdnav_get_disk_region_mask()` → `dvdnav_set_region_mask()` (§5.8), angle default.
 
### 5.3 PS/PES router (dv_ps.c)
- Parse pack header (MPEG-2 and MPEG-1 forms), system header, PES headers, PTS/DTS.
- Stream IDs: `0xE0` video; `0xC0–0xC7` MPEG audio; private_stream_1 (`0xBD`) sub-IDs: `0x20–0x3F` SPU, `0x80–0x87` AC-3, `0x88–0x8F` DTS, `0xA0–0xA7` LPCM (skip/parse its header bytes per VLC/FFmpeg); `0xBF` nav (libdvdnav already consumed it).
- Only forward the currently selected audio/SPU substreams (from `dvdnav_get_active_audio_stream()` / `dvdnav_get_active_spu_stream()` mapped to physical IDs, as VLC does).
### 5.4 Video (dv_video.c, dv_render.c)
- libavcodec `mpeg2video` (handles MPEG-1 too), `thread_count = 1` initially (lowest latency for menus; revisit).
- Frame queue holds decoded frames with PTS and repeat/field flags. Presentation honours `repeat_pict`/RFF so 23.976 film material with pulldown displays correctly at 59.94.
- Output `RETRO_PIXEL_FORMAT_XRGB8888` via swscale (BT.601 matrix, limited range). Geometry base 720×480/576, `max_width/height` 720×576.
- Aspect: `geometry.aspect_ratio` from the stream's DAR (4:3 or 16:9) — never from pixel dimensions. Core option: auto / force 4:3 / force 16:9 / letterbox 16:9 on 4:3 display.
- Deinterlacing (core option): off / bob (field-rate output) / yadif or bwdif via libavfilter (M6). Default: off for progressive-flagged frames, simple blend otherwise.
- Still frames: re-present the last frame each `retro_run` (or pass `NULL` to `video_cb` to dupe when nothing changed and overlay is unchanged).
### 5.5 SPU decode, highlight & compositing (dv_spu.c, dv_overlay.c)
- Reassemble SPU packets by total size (first 2 bytes), then parse the DCSQ chain: `FSTA_DSP` (forced), `STA_DSP`, `STP_DSP`, `SET_COLOR`, `SET_CONTR`, `SET_DAREA`, `SET_DSPXA`, `CHG_COLCON` (store; rarely used, implement later).
- Decode RLE into an **8-bit index buffer (values 0–3)** at display-area size; keep the 4 color indices + 4 alpha values separately.
- **Highlight (the menu-critical piece)** — port VLC `ButtonUpdate()`:
  - `pci = dvdnav_get_current_nav_pci()`, `dvdnav_get_current_highlight(&btn)`, `dvdnav_get_highlight_area(pci, btn, mode, &hl)` where mode 0 = selected, 1 = activated.
  - For i in 0..3: `clut_index = (hl.palette >> (16 + 4*i)) & 0xF`, `alpha = ((hl.palette >> (4*i)) & 0xF) * 255 / 15`, color = CLUT[clut_index].
  - Inside rect (`hl.sx,hl.sy`)–(`hl.ex,hl.ey`) use highlight palette/alpha; outside use the SPU's own `SET_COLOR`/`SET_CONTR`.
- CLUT entries are `0x00YYCrCb`; convert to RGB once per CLUT change (BT.601), cache 16 RGBA entries.
- Visibility rules: in menus (or when the highlight is active) display menu SPU regardless of the user's subtitle toggle; for titles honour subtitle on/off but always show **forced** SPUs.
- Composite after scaling to RGB onto the output frame, scaling SPU coordinates with the video (SPU coordinates are in 720-wide stored-frame space even for anamorphic content).
- Changing the selected button must **only** re-run the palette step — no re-decode. Target: button move redraw < 1 ms.
### 5.6 Clock & A/V sync (dv_clock.c)
libretro inverts the usual player model: the frontend calls `retro_run` at the display rate and expects one video frame and a matching slice of audio. DeeVee therefore owns a **monotonic presentation clock** advanced by `1/fps` per `retro_run`.
 
- `fps` = 60000/1001 for NTSC VTS, 50 for PAL; switch via `SET_SYSTEM_AV_INFO` on VTS change (rare). `sample_rate` = 48000 always.
- Each `retro_run`: pull blocks until the video queue has a frame with timeline PTS ≥ clock (or still/wait state), present the newest frame ≤ clock, emit exactly `48000/fps` (fractional-accumulated) audio samples from the audio ring buffer; pad with silence on underrun, drop on overrun beyond a threshold.
- **Discontinuities:** DVD PTS resets at cell/VOBU boundaries (seamless branching, menus, angle changes). Maintain `timeline = stream_pts + offset`. On `NAV_PACKET`, if `vobu_s_ptm` ≠ previous `vobu_e_ptm` (VLC/FFmpeg `dvdvideodec.c` both do a version of this), recompute `offset` so the new VOBU starts where the previous ended. On `HOP_CHANNEL` hard-reset the clock to the next decoded frame.
- Use `RETRO_ENVIRONMENT_GET_THROTTLE_STATE` to handle fast-forward/slow-motion gracefully (skip audio, decode-drop B-frames under fast-forward).
- Optional: `SET_AUDIO_BUFFER_STATUS_CALLBACK` to nudge sync.
### 5.7 Audio (dv_audio.c)
- Decoders: `ac3`, `dca`, `pcm_dvd`, `mp2`/`mp1`. Set AC-3 `drc_scale` from a core option (night mode).
- swresample: any layout/rate → stereo s16 48 kHz (Dolby Pro Logic II–compatible downmix matrix option).
- On stream switch, flush decoder + ring buffer; crossfade not required.
### 5.8 Region & CSS
- **VM region (RCE):** at open, `mask = dvdnav_get_disk_region_mask()`; pick the lowest region the disc allows (or the user's forced region from options) and call `dvdnav_set_region_mask(1 << (r-1))`. This defeats RCE checks performed by the disc's VM program. It is independent of the drive's region.
- **Drive region (RPC-2):** libdvdcss can usually recover title keys without the drive's region matching. Document that RPC-2 drives which refuse to read sectors from a mismatched-region disc can't be fully worked around in software.
- **No libdvdcss is a normal, supported setup:** most users rip with MakeMKV etc., so their images are already decrypted. With no libdvdcss present the core must open and play unencrypted images/folders silently (no warning OSD); only when a title is actually scrambled does it show "This disc is encrypted — place libdvdcss in RetroArch's system folder".
- **libdvdcss location (D-024):** treated like a BIOS file — the user places it in RetroArch's system directory (`<system>/libdvdcss.so.2`, `libdvdcss-2.dll`, `libdvdcss.2.dylib`). Listed as an optional `firmware` entry in `deevee_libretro.info` so it appears in RetroArch's Core Information → Firmware screen. At `retro_load_game` the core pre-loads it by absolute path (`dlopen(..., RTLD_NOW|RTLD_GLOBAL)` / `LoadLibraryW`) before `DVDOpen`, so libdvdread's own by-name dlopen resolves to the already-loaded library (glibc matches by soname, Windows by module name) — no libdvdread patch needed if that holds; otherwise a small patch adding a path hook. Android: system dir is usually on noexec storage, so the core copies the file into app-private storage first (verify at M7). Not loadable on iOS/tvOS (no unsigned dlopen) — CSS unsupported there.
- **Encrypted ISOs:** libdvdcss can crack title keys on image files; key cache under the system dir avoids repeated slow cracking. Show an OSD message ("Retrieving CSS keys…") because first open can take seconds.
### 5.9 Input (dv_input.c)
Default RetroPad mapping (configurable later via input descriptors):
 
| Button | In menu | During title |
|---|---|---|
| D-pad | `dvdnav_{upper,lower,left,right}_button_select` | Left/Right: seek ±10 s; Up/Down: ±60 s |
| A | `dvdnav_button_activate` | Play/Pause |
| B | `dvdnav_go_up` (Return) | Return / go up |
| Start | `dvdnav_menu_call(DVD_MENU_Root)` | Root menu |
| Select | Title menu (`DVD_MENU_Title`) | Toggle OSD |
| L / R | — | Previous/next chapter (`dvdnav_prev_pg_search` / `next_pg_search`) |
| X | — | Cycle audio stream |
| Y | — | Cycle/toggle subtitles |
| L2 / R2 | — | Rewind / fast-forward (sector search steps) |
| L3 | — | Cycle angle |
 
- Edge-triggered input (on press, with auto-repeat for d-pad after 400 ms).
- **Mouse/pointer:** map frontend coordinates → stored-frame coords (undo aspect & letterboxing) → `dvdnav_mouse_select(pci, x, y)` on move, `dvdnav_mouse_activate` on click/tap.
- Pause: libretro has no pause callback to the core; implement play/pause internally (stop advancing the clock, output silence).
### 5.10 Core options (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2)
Region (Auto/1–8), Menu language, Audio language, Subtitle language, Subtitles default (auto/on/off — 'forced-only' merged into off, D-015), Aspect (auto/4:3/16:9), TV Display Type (16:9 / 4:3 LB / 4:3 P&S, M6b), Deinterlace, Audio downmix (stereo/Pro Logic II), Dynamic range compression, ~~Parental level~~ (D-019), End-of-disc behaviour, CSS method (key/disc/title), Show OSD. Implemented (M6): all of the above except Parental level (D-019); Aspect lacks the letterbox variant (D-023 → now M6b, D-025).
 
### 5.11 OSD
Use `RETRO_ENVIRONMENT_SET_MESSAGE_EXT` for: title/chapter on change, time on seek, audio/subtitle track name+language on change (`dvdnav_audio_stream_to_lang`, `dvdnav_spu_stream_to_lang`), CSS key retrieval progress, errors.
 
### 5.12 Save states / resume
- v1: `retro_serialize_size` returns a small fixed size storing a **resume point** (title, chapter, sector/time, selected audio/SPU, SPRMs where accessible). Loading jumps there via `dvdnav_title_play`/`part_play` + sector search. Mark serialization quirks accordingly. Investigate libdvdnav's internal VM state save/restore functions in 7.0 before designing further.
- Not a goal: frame-exact rewind/run-ahead/netplay.
---
 
## 6. Build & dependencies
 
- Repo layout:
  ```
  deevee/
    src/                 core sources (§3)
    deps/libdvdcss/      git submodule (pin 1.5.0)
    deps/libdvdread/     git submodule (pin 7.0.1)
    deps/libdvdnav/      git submodule (pin 7.0.0)
    deps/ffmpeg/         git submodule (pin a release tag, e.g. 7.1.x or 8.x)
    deps/libchdr/        git submodule (M5)
    libretro-common/     git submodule
    Makefile, Makefile.common, link.T, jni/ (Android), .gitlab-ci.yml
    deevee_libretro.info
    tests/
  ```
- The three libdvd* libraries now build with **Meson** (autotools removed as of the 2025 releases). Build them as static libraries in a pre-step, then link into the core.
- **CSS build switch (D-003):** the Makefile exposes `DEEVEE_CSS=static|dlopen|none` from M0 onward.
  - `static` (**default until M7**): libdvdcss built from `deps/libdvdcss` and linked into libdvdread and the core. Encrypted discs/ISOs work with zero setup.
  - `dlopen`: libdvdread is built *without* a libdvdcss link and loads it at runtime if present (libdvdread's native fallback behaviour). The `deps/libdvdcss` submodule is not compiled. Encrypted content works only when the user supplies libdvdcss; unencrypted discs always work.
  - `none`: no CSS support at all (CI fixtures, testing the "missing library" path).
  - The core reports which mode it was built with in the log and in `retro_system_info.library_version` (e.g. `0.1.0 (css:static)`).
- **Rule while in `static` mode:** no DeeVee source file may call libdvdcss directly or include its headers. All CSS goes through libdvdread, so the switch never touches core code. Set `DVDCSS_CACHE`/`DVDCSS_METHOD` via environment variables only (they're honoured in both modes).
- FFmpeg configure (static, PIC):
  `--disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect --enable-decoder=mpeg2video,mpeg1video,ac3,dca,pcm_dvd,mp1,mp2 --enable-parser=mpegvideo,ac3,dca,mpegaudio --enable-swscale --enable-swresample` (+ `--enable-avfilter --enable-filter=yadif,bwdif` at M6).
- Symbol hygiene: export only `retro_*` (`link.T` version script), `-fvisibility=hidden`.
- Targets (in order): Linux x86_64 → Windows (MinGW-w64) → macOS (arm64/x86_64) → Android (NDK, arm64-v8a) → others. Follow the libretro GitLab CI templates for buildbot integration (§M7).
- `deevee_libretro.info`: `display_name = "Media Player (DeeVee)"`, `corename = "DeeVee"`, `supported_extensions = "iso|ifo|img|chd"`, `needs_fullpath = true`, `license = "GPLv2+"`, `categories = "Media Player"`.
---
 
## 7. Milestones & acceptance criteria
 
### M0 — Skeleton & dependencies
- Repo, submodules, Makefile building static deps + core on Linux, with the `DEEVEE_CSS` switch in place (default `static`; `none` must also build).
- Core loads in RetroArch, opens an ISO/VIDEO_TS via libdvdnav, logs volume ID, region mask, title/VTS count; renders a black frame; no crashes on unload.
- **Done when:** `retroarch -L deevee_libretro.so disc.iso --verbose` shows disc info in the log.
### M1 — Video-only title playback
- Port `examples/menus.c`-style loop; PS router; mpeg2video decode; swscale to XRGB; fixed 59.94/50 fps clock; aspect from stream; RFF handling.
- Start in First Play PGC (menus will show as video, non-interactive).
- **Done when:** a 4:3 NTSC and a 16:9 PAL disc play with correct aspect and smooth motion for 10 min.
### M2 — Audio & sync
- AC-3, LPCM, MP2, DTS decode → 48 kHz stereo; audio ring buffer; clock + VOBU discontinuity handling; HOP_CHANNEL/flush.
- **Done when:** lip-sync within ±1 frame over a full feature, including across layer break and seamless-branching cells; no audio pops at cell changes.
### M3 — SPU (subtitles)
- SPU reassembly/parse/RLE; CLUT change handling; forced subtitles; subtitle selection by language; composite.
- **Done when:** subtitles match VLC visually (position, colour, timing) on 3 test discs, including forced-only tracks.
### M4 — Interactive menus (core value)
- Highlight rect/palette (VLC `ButtonUpdate` port), RetroPad navigation, activation, `go_up`, menu calls, still frames (timed + infinite), WAIT, mouse/pointer.
- Auto-action buttons and buttons spanning multiple SPU groups (letterbox/widescreen) handled.
- **Done when:** test set menus (§8) navigate identically to VLC: correct highlight on every button, every button reachable, activation lands in the right PGC, motion menus loop with audio, still menus wait for input.
### M5 — Region, CSS, languages, content sources
- Region auto/forced; RCE disc test; libdvdcss key cache; mounted physical drive path; language preference options.
- **CHD support** (added 2026-10-15 at user request): libchdr submodule, `.chd` stream source, `chd` in extensions/.info.
- **Done when:** an RCE region-1 disc and a region-2 disc both reach their main menus; a physical encrypted disc plays from a mounted drive on Linux and Windows; a DVD CHD made with `chdman createdvd` from a test ISO plays identically to the ISO (same video/audio hashes in the harness).
### M6 — Polish & control
- Chapter skip, seek (VLC-style sector-based rather than pure time search), angles, OSD, deinterlace filters, aspect overrides, fast-forward behaviour, end-of-disc behaviour, core options v2 complete.
- **Done when:** all §5.9 controls work; no seek lands more than ~2 s from target; deinterlace options visibly correct on interlaced material.
### M6c — TIMERIC menu parity with VLC (added 2026-10-16, user request)
- User reports some TIMERIC.iso menu navigation isn't 1:1 with VLC (a known-hard disc that has stumped other players). Method: drive VLC and DeeVee with the same scripted key sequences from the same start state; at each step compare domain/title/PGC/cell, button number and SPRM/GPRM state (VLC: `--verbose=2` dvdnav logs + `-V yuv` frame dumps; DeeVee: harness trace), then diff the first divergence and fix. Each fixed case becomes a regression test in `tests/`.
- **Done when:** every menu path reachable from TIMERIC's root menu lands on the same PGC/button as VLC, with the same highlight picture.
### M7 — Portability, performance, distribution
- **Carried over from M5:** verify CSS decryption on genuinely encrypted media (key cracking, cache reuse, CSS method option, slow-open OSD); mounted physical drive playback (Linux, Windows).
- **CSS break-out for distribution (D-003 phase 2, D-024):** switch distributed builds to `DEEVEE_CSS=dlopen`, loading libdvdcss from RetroArch's system directory like a BIOS (§5.8). Add the optional firmware entries to the .info file. Verify the pre-load trick per platform (Linux `libdvdcss.so.2`, Windows `libdvdcss-2.dll`, macOS `libdvdcss.2.dylib`, Android copy-to-private-storage) and document it. When an encrypted disc is opened without libdvdcss, show a clear OSD message ("This disc is encrypted — libdvdcss not found; see docs") instead of a generic read error. Keep `static` as the default for local/dev builds.
- **Per-platform builds:** a libretro core is a native shared library, so each OS/CPU pair needs its own binary (Linux x86_64 `.so`, Windows x86_64 `.dll`, macOS x86_64/arm64 `.dylib`, Android arm64-v8a/armeabi-v7a `.so`, Linux arm64/armhf for Pi-class devices). One source tree; the Makefile `platform=` switch + buildbot produce them. Windows/macOS/Android builds; VFS-backed stream I/O on Android; optional read-ahead thread for physical drives; resume-point save states; optional GL hardware path (YUV→RGB + SPU blend in shader) for low-power devices; libretro buildbot CI; docs page for docs.libretro.com.
- **Done when:** full test set passes on 3 desktop OSes + Android; Raspberry Pi 4–class device plays without drops; a `dlopen` build plays unencrypted fixtures with no libdvdcss present, plays encrypted test discs once the user supplies it, and shows the "libdvdcss not found" message otherwise.
### M6b — Player display type (added 2026-10-16, user request; supersedes D-023)
- Core option **TV Display Type**: `16:9 Widescreen` (default) / `4:3 Letterbox` / `4:3 Pan & Scan`, like a hardware player's setup menu. Sets SPRM 14 (display mode + "TV aspect" bits) via libdvdnav before/at open so the disc's VM picks the matching menus, and selects the matching SPU variant (wide / letterbox / pan-scan streams from `SPU_STREAM_CHANGE`).
- Rendering for 16:9 content on a 4:3 TV: **Letterbox** — output 4:3, picture scaled into the middle with black bars (SPU + highlight coords mapped through the same transform, pointer mapping updated); **Pan & Scan** — 4:3 crop using the MPEG-2 sequence-display-extension / pan-scan offsets when present, centre crop otherwise. 4:3 content is unchanged in all modes.
- Keeps the existing *Aspect Ratio* override (Auto/4:3/16:9) for mis-flagged discs.
- **Done when:** on CHRIMBUS / IDIOCRACY (16:9) each mode shows the right geometry, menus highlights land on buttons in all three modes (pad + pointer), letterbox subtitles sit inside the picture, and a disc with per-mode menus (if found) switches menus.
### M8 (stretch) — Extras
- Closed captions (Line 21), `CHG_COLCON` support, DVD-VR, karaoke audio mixing modes.
---
 
## 8. Test corpus & methodology
 
Build a personal (non-redistributed) test set covering:
- NTSC 4:3, NTSC 16:9 anamorphic, PAL 16:9; film (pulldown) and video (interlaced) sources.
- Motion menus with audio; still menus; menus with many buttons and odd navigation graphs.
- Interactive/game DVDs (e.g. quiz or "choose your path" discs) — stress the VM and still frames.
- Multi-angle discs; seamless branching (director's cut); dual-layer with layer break.
- RCE-protected region 1 disc; region 2/4 discs; region 0 home-authored disc.
- DTS + LPCM + AC-3 tracks; forced subtitles; letterbox/pan-scan SPU variants.
- Unencrypted homebrew DVD built with dvdauthor (reproducible, sharable test fixture for CI).
Methodology:
- **Behavioural oracle:** VLC 3.0.x. For each disc, record a menu walkthrough script (button sequence → expected destination) and verify DeeVee matches.
- **Pixel spot-checks:** compare DeeVee frame dumps against FFmpeg (`-f dvdvideo` for titles/menu assets) and VLC snapshots.
- **Headless harness (tests/):** a minimal C frontend that loads the core, injects scripted input, and dumps frames/audio hashes — enables CI regression on the dvdauthor fixtures.
---
 
## 9. Risk register
 
| Risk | Impact | Mitigation |
|---|---|---|
| Time-based seeking in libdvdnav is imprecise (noted by mpv maintainers) | Bad seek UX | Use sector-based seeking like VLC; time display from cell/VOBU times. |
| PTS discontinuities in seamless branching | A/V drift, stutter | Offset model in §5.6; test with branching discs early (M2). |
| libretro fixed-cadence model vs. variable media timing | Judder | Present by timeline PTS with RFF; DRC in RetroArch handles 59.94 vs 60 Hz. |
| libdvdcss distribution concerns on buildbot | Can't ship CSS-capable core | D-003: static during development, dlopen for distribution (M7). |
| `dlopen` path rots while unused during M0–M6 | Surprise breakage at M7 | CI builds `DEEVEE_CSS=none` on every commit (same libdvdread config minus the library), so the no-direct-link path stays compiling. |
| Physical drive seek latency stalls `retro_run` | Audio dropouts | Read-ahead thread for device sources only (M7). |
| SPU edge cases (CHG_COLCON, multiple display areas) | Wrong menu visuals on some discs | Log unknown commands; implement in M8; VLC behaviour as oracle. |
| FFmpeg API churn | Build breakage | Pin a release tag; wrap avcodec calls in one module. |
 
---
 
## 10. Coding conventions
- C99, no C++ in core code (eases console ports). Use libretro-common helpers (`file_stream`, `rthreads`, `retro_timers`).
- One module per subsystem; no globals outside `libretro.c` (pass a `dv_ctx_t*`).
- Every ported function gets a comment: `/* Ported from VLC modules/access/dvdnav.c ButtonUpdate() @ <commit> */`.
- Logging via `retro_log_printf` with `[DeeVee]` prefix; verbose logging behind a core option.
---
 
## 11. Upstream versions (verified 2026-10-15)
- libdvdnav **7.0.0**, libdvdread **7.0.1**, libdvdcss **1.5.0** (VideoLAN, released late 2025; Meson build).
- FFmpeg: pin latest stable release at M0 time.
- **Re-checked at M0 start (2026-10-15), pinned:** libdvdcss **1.6.0**, libdvdread **7.1.1**, libdvdnav **7.0.0**, FFmpeg **n8.1.3** (see D-007), libretro-common master @ submodule commit.
- VLC: reference `3.0.x` stable branch; consult `master` for refactored highlight code.
Re-check versions at the start of M0 and M7.
 
---
 
## 12. Status board
 
| Milestone | Status | Notes |
|---|---|---|
| M0 Skeleton | ☑ Done (2026-10-15) | Repo, submodules, Makefile (`static`/`dlopen`/`none` all build + play the fixture), libretro.c/dv_source.c/dv_nav.c, `tests/harness.c`, dvdauthor fixture, `make check`. Acceptance passed in Steam RetroArch (VFS path) on the fixture and on `TestISOs/IDIOCRACY_SIDEA.iso` (CSS keys OK, region-1, 11 titles/6 VTS). Build env: distrobox `deevee-build`; run RetroArch via `SteamLinuxRuntime/steam-runtime/run.sh ./retroarch.sh` from its dir (needs libCg). **Next: M1.** |
| M1 Video | ☑ Done (2026-10-15) — PAL real-disc check pending | dv_ps/dv_video/dv_render/dv_clock + nav fill/service loop; VLC still handling (seq-end + vobu_se_e_ptm flush); timed/infinite stills; WAIT drains to clock; geometry/timing from first picture, SET_GEOMETRY / SET_SYSTEM_AV_INFO on change. Acceptance: IDIOCRACY 16:9 NTSC film feature 10 min (f10478→f46500) 0 drops, 0 errors, 2/3 pulldown cadence; 4:3 NTSC trailer + fixture; PAL 16:9 only on the synthetic `pal169` fixture (no real PAL disc available). TIMERIC + CHRIMBUS run clean (menus wait correctly). ~1.3 ms/tick on the Deck. **Temporary:** A = activate button / skip still (dv_nav_activate) until M4. |
| M2 Audio/sync | ☑ Done (2026-10-15) — subjective lip-sync check by user pending | dv_audio (AC-3/DTS/LPCM/MP2 → 48 kHz stereo, decoder downmix), PTS-chunk queue + per-tick render, compressed video FIFO + per-stream read gating (D-012..D-014). A/V offset on fixtures, both sides of a cell join: LPCM 0.0–0.6 ms, AC-3 0.1–1.8 (one +6.1 at the join), MP2 −2.0–0.6, DTS = fixture's own +10.7; no clicks at cell joins. IDIOCRACY full 84-min feature (dual-layer): 0 video drops/errors/resyncs, 0 audio errors, 5 real audio discontinuities (title/menu transitions); layer break is PTS-continuous. TIMERIC/CHRIMBUS menu audio correct. Not yet: internal pause, throttle-state handling (M6). |
| M3 SPU | ☑ Done (2026-10-15) — needs more real discs with subtitle/forced tracks | dv_spu (VLC reassembly/DCSQ port, FFmpeg RLE), dv_overlay (CLUT→RGB, blend, highlight hook for M4), core options v2 started (Subtitles auto/on/off, Subtitle Language). `subs` fixture: positions/colours exact, forced-only when off, on/off on the exact tick, language selection works. IDIOCRACY feature subtitles vs FFmpeg `-f dvdvideo` + dvdsub overlay oracle: picture MSE 0.4, subtitle-area MSE 1.5, glyph pixels identical (2582/2582). Tim & Eric discs: full-screen forced menu SPUs decode cleanly (highlights = M4). Only one real disc with title subtitles so far; no real forced-subtitle disc yet. |
| M4 Menus | ☑ Done (2026-10-15) — user walkthrough in RetroArch passed: SPU/highlights correct on 4:3 (TIMERIC) and 16:9 (CHRIMBUS), sub-menus work, subtitle selection in the disc menu works | Highlight (VLC ButtonUpdate port, D-017) on the *presented* PCI (D-018); dv_input: full §5.9 RetroPad menu set (D-pad w/ 400 ms repeat, A activate/skip still, B Return, Start root, Select title, L/R chapter), pointer hover/click; user jumps hard-flush queues (instant). Verified: CHRIMBUS 3 buttons — vs VLC (yuv vout oracle) luma identical in highlight (mean diff 0.93/255), alignment shift (0,0), only VLC's 4:2:0 chroma edges differ; IDIOCRACY highlights appear exactly when buttons go live (f9334), Play/Start/Select OK; TIMERIC pointer hover + click activate OK. Not verified: multi-group (wide/letterbox) button sets (left to libdvdnav), activation-colour flash (not shown: flush is immediate), full per-disc walkthrough vs VLC. |
| M5 Region/CSS | ◐ Done except hardware-dependent items (2026-10-15) | Region auto/forced + RCE fixture, Menu/Audio/Subtitle language, CSS method, key cache (verified written), CHD (== ISO on fixtures and CHRIMBUS, +7 ms CPU/s). **Deferred to M7 (user has no encrypted media or USB drive right now):** real CSS decryption on an encrypted image/disc — note (2026-10-16): all four TestISOs are *already-decrypted* backups (0 scrambled packs in a sector sample; libdvdcss cached all-zero title keys), so CSS cracking itself is still untested; mounted physical drive playback on Linux/Windows. |
| M6 Polish | ☑ Done (2026-10-15) — user walkthrough pending; angle untested (no multi-angle disc) | Title controls per §5.9: D-pad seek ±10/±60 s, A pause, X/Y audio/subtitle cycle, L3 angle, L2/R2 hold-scan, Select info, L/R chapters, Start/B. Seek = `dvdnav_jump_to_sector_by_time` (TMAP/admap) from the *presented* time: 24 seeks across IDIOCRACY mean \|error\| 0.44 s, max 0.92 s. Deinterlace (libavfilter bwdif/yadif, field rate, interlaced-flagged only): TIMERIC 232 decoded → 460 presented, combing visibly removed. Options complete except the letterbox aspect variant (D-023): Deinterlace, Aspect, Downmix (stereo/DPL II), DRC (normal/off/night), End of disc (restart/stop/close), Show OSD. OSD via SET_MESSAGE_EXT. Fast-forward skips B-frames (throttle state). |
| M6b Display type | ☑ Done (2026-10-16) — RetroArch walkthrough by user pending | `deevee_tv_display` 16:9 / 4:3 Letterbox / 4:3 Pan & Scan. libdvdnav patch (patches/libdvdnav/0001, D-026) sets SPRM 14 and exposes per-mode SPU stream; IFO permitted display modes honoured (D-027); per-mode button groups for highlight + pointer; letterbox/P&S transform after (wide SPU) or before (native LB/P&S SPU variant) compositing. Verified: CHRIMBUS + IDIOCRACY menus are flagged *P&S only* → P&S with the disc's own P&S SPU stream (1) and button group, highlight on the right text, pointer hover hits buttons 1–3; IDIOCRACY feature is *LB only* → letterbox, bars exactly black, round Earth; pal169 fixture all 3 modes (make check). Cost +0.5 ms/frame on the Deck. Not verified: runtime option switch in RetroArch, a disc with real per-TV-type menus (SPRM 14 branching), pan-scan offset vectors (none on test discs). |
| M7 Portability | ◐ Started (2026-10-16) | **libdvdcss from the system folder (D-024) done on Linux:** non-static builds pre-load `<system>/libdvdcss.so.2` (RTLD_GLOBAL; libdvdread's by-name dlopen then resolves to it — verified with a shared libdvdcss build), else the OS's copy, else none. No libdvdcss = normal: decrypted rips play silently (libdvdread's "Encrypted DVD support unavailable" downgraded to INFO). Scrambled PES (PES_scrambling_control still set after libdvdread) are dropped, reading halts, and an alert OSD (shown even with Show OSD off) names the file to install; `scrambled.iso` fixture (flags set on ntsc43) in make check. .info lists the three per-OS file names as optional firmware. make check passes in static/dlopen/none. Windows LoadLibraryW path written, untested; macOS dyld leaf-name match and Android (noexec system dir → copy to core dir) still to do. Real CSS cracking still untested (no encrypted media). |
| M8 Extras | ☐ | |
 
---
 
### Session notes / gotchas
- `dvdnav_get_next_cache_block()`: the caller must pass a real 2048-byte buffer in `*buf` — event payloads are written into it; only `BLOCK_OK` repoints `*buf` into the cache. Free with `dvdnav_free_cache_block()` only when `buf != mem` (as `examples/menus.c`).
- `dvdnav_open_stream2()` / `DVDOpenStream2()` pass the **same** `priv` to the logger and the stream callbacks, so `priv` is always a `dv_stream_t*` (which carries `ctx`).
- Stream `pf_seek` must return 0 on success (not the offset).
- Harmless upstream log noise on file sources: "Device name string NULL", "Can't read name block", `findDirFile ... video_ts/`.
- `libdvdnav: Suspected RCE Region Protection!!!` (raw stdout from vm/decoder.c) fires whenever a disc's VM reads SPRM 20. Informational. IDIOCRACY_SIDEA (region 1) triggers it and passes with VM region 1 — reuse as the M5 RCE test.
- Repo is **local-only** for now (no remote; user decision 2026-10-15). Don't push or set up hosted CI; run `make check` + `make DEEVEE_CSS=none` locally instead of the CI job in the §9 risk register.
- **Never pass `--appendconfig` to the user's Steam RetroArch**: `config_save_on_exit = true` writes the appended settings into its `retroarch.cfg` (2026-10-15 this silently set `audio_driver = "null"`; original is `"pulse"`). For silent/automated RA runs, copy `retroarch.cfg` to the scratchpad, edit the copy, and launch with `--config <copy>`.
- Test media lives in `TestISOs/` (gitignored, never commit).
- Most commercial discs only put PTS on a few pictures per GOP (IDIOCRACY: ~90 % of frames interpolated). `dv_video` interpolates `next_pts = pts + duration`, duration = frame period × (2+repeat_pict)/2.
- Menu buttons can be absent during a menu's intro animation (`hli_ss 0, btn_ns 0`): activation does nothing until highlight info appears — correct, VLC behaves the same.
- Test discs (TestISOs/): **IDIOCRACY_SIDEA** — region 1, RCE check, 2× 7 s timed stills, 4:3 trailer, 16:9 motion main menu (buttons appear ~f9500), Play → intro title 9 → 7 s notice → title 1 film feature with pulldown. **TIMERIC** — region-free, 25 VTS, VMGM is a still picture + ~294 s audio loop. **TIM_AND_ERIC_CHRIMBUS_SPECIAL** — region 1, intro then VTSM menu with music then infinite still.
- Harness: `HARNESS_INPUT="9500:A"`, `HARNESS_DUMP=<dir>` + `HARNESS_DUMP_FRAMES` / `HARNESS_DUMP_EVERY`; log lines are prefixed `[f<frame>]`. Useful IDIOCRACY feature run: `HARNESS_INPUT=9500:A`, 46500 frames.
- VLC 3.0.x reference sources are sparse-checked-out in `refs/vlc` (gitignored) @ 843607a066be.
- **A/V sync fixtures** (`avs_ac3` 5.1 centre-only beep, `avs_lpcm`, `avs_mp2`, `avs_dts`): 2-frame flash + 100 ms beep every 30 frames, two VOBs (cell join at ~10 s). `HARNESS_AVSYNC=1` (2 = per event + click times) reports audio−video offset and clicks; `HARNESS_WAV=<file>` dumps output audio. Gotchas found building them: ffmpeg's `volume` filter `enable=` is evaluated per *audio frame* (use `sine=...:samples_per_frame=16`); ffmpeg's DVD muxer stamps LPCM PTS on alternate packets (PTS marks the first access unit, not the packet start); the experimental `dca` encoder adds ~10.7 ms of delay — ffmpeg itself measures avs_dts at +10.7 ms.
- `pcm_dvd` wants the packet to start at the 3-byte LPCM header: skip id + frame count + first-AU pointer (4 bytes), like AC-3/DTS.
- `dvdnav_get_active_audio_stream()` returns the **physical** stream (0–7); match it against substream id & 7.
- `dvdnav_get_active_spu_stream()` returns **int8_t**: a hidden stream (bit 7 = subtitles off, forced only) comes back negative (0x80 → −128); only −1 means none. Mode 0 = the 'wide' variant for 16:9.
- `dvdnav_set_active_stream()` takes the **logical** stream; `dvdnav_spu_stream_to_lang()` too.
- **FFmpeg oracle**: `build/ffmpeg-cli/ffmpeg` (n8.1.3 CLI with libdvdnav/libdvdread incl. CSS; built by hand in the container, see M3 session). Use `-nostdin` (it hangs in background shells otherwise) and don't `-ss` on `-f dvdvideo` (lands seconds off) — decode from title start and `select='between(t,a,b)'`. Compare with numpy (python3-numpy installed in the container).
- Subtitle fixture `subs` (stream 0 en: white/green + forced yellow; stream 1 fr: magenta; display off by default); PNGs from `tests/fixtures/make_spu_png.py` + spumux.
- **VLC as oracle** (Flatpak 3.0.23, has libdvdcss): always `--ignore-config --no-qt-privacy-ask -I rc --rc-fake-tty --no-audio --no-video-title-show`, commands via a stdin pipe (`snapshot`, `key key-nav-right`, `quit`). Flatpak has a private /tmp → write outputs under `build/vlc/`. **Snapshots with `--vout=vdummy` omit palettised DVD SPUs/highlights** (RGBA text like CC is included) — use `--vout=yuv --yuv-file=X.y4m --yuv-yuv4mpeg2 --yuv-chroma=I420` to get frames after VLC's SPU blend. Compare luma for shape/alignment; VLC blends SPU chroma at 4:2:0 so edges differ in colour. Scripts used: scratchpad `cmp_y4m.py` (rebuild as needed).
- **RetroArch calls `retro_set_input_state` (and other callbacks) only *after* `retro_load_game`, possibly several times** — never cache callbacks at load. The harness now mimics this order. (Bug found 2026-10-15: all input was dead in RetroArch while harness tests passed.)
- For automated RetroArch runs use a copied config: `cp retroarch.cfg <scratch>/ra-test.cfg`, set `audio_driver = "null"`, `config_save_on_exit = "false"` **and `core_options_path = "<scratch>/ra-test-core.opt"`** (RetroArch saves `config/DeeVee/DeeVee.opt` on exit regardless of config_save_on_exit) in the copy, launch with `--config <scratch>/ra-test.cfg`.
- Harness: `HARNESS_POINTER="frame:x,y,frame:x,y:press"` (720×480 pixels) for mouse/touch tests.
- Menu SPUs on both Tim & Eric discs: full-screen 720×478, forced, no stop, base alpha 0 (only highlights visible). CHRIMBUS buttons: 1 Play (234,399)-(262,428), 2 Special Features, 3 Setup. IDIOCRACY main menu: highlight is a yellow tint; buttons live from ~f9334 on first entry.
- **CHD**: libchdr pinned at upstream HEAD 47d7941 (2026-10-11; newer than tag v0.3.0). Built from `unity.c` **plus** `src/libchdr_codec_avhuff.c` (missing from upstream's unity build) with its own flags; **its include dir must precede libretro-common's**, which ships an older copy of the libchdr headers. `chdman` (container package `mame-tools`, 0.264) creates test CHDs: `chdman createdvd -i x.iso -o x.chd` (hunk 4096 = 2 sectors, `DVD ` metadata). `make check` asserts CHD playback == ISO playback.
- **Test ISOs are not CSS-encrypted** (libdvdcss DEBUG shows title keys `00:00:00:00:00` from cache): the actual decryption path is untested until an encrypted image or physical disc is available.
- Region fixture `region2`: VMG category byte 0x23 patched to 0xFD (region 2 only) + First Play `if (s20 == 2) jump title 1; jump title 2;` (dvdauthor accepts `s20`).
- No libdvdnav API for parental level (PTL_REG = 15 = unrestricted, set in vm_reset): parental option dropped (D-019).
- libdvdnav exposes no VTS count; `dv_nav_log_disc_info` reads it from the VMG IFO via a short-lived libdvdread handle.

### Plan update (2026-10-16)
- User rips with MakeMKV: all TestISOs are decrypted; no encrypted image available this session — CSS-cracking verification stays deferred; test the no-libdvdcss and libdvdcss-in-system-dir paths with the decrypted images plus a synthetic scrambled-flag fixture for the "encrypted" OSD message.
- Order agreed: **M6b (TV Display Type) → D-024 libdvdcss-from-system-dir (dlopen build) → M6c TIMERIC VLC parity → rest of M7.**

### Session gotchas (2026-10-16)
- All DEEVEE_CSS modes link to `deevee_libretro.so`; Makefile now relinks on a mode switch (`build/.css-mode`). Copy to `deevee_<mode>_libretro.so` if you need several side by side.
- Test libdvdcss for the system-folder path: `meson setup <dir> deps/libdvdcss --default-library=shared` in the distrobox, copy `src/libdvdcss.so.2` into a scratch system dir. Don't install it into the user's RetroArch system folder.
- 16:9 commercial menus are typically flagged *pan&scan only* in the IFO; features *letterbox only*.

### Previous session (left off 2026-10-15)
- M0–M6 done and committed. M6 awaits the user's RetroArch walkthrough (seek/pause/tracks/info/deinterlace on TIMERIC + IDIOCRACY).
- **Before starting M7, ask the user which M7 items to prioritise** — several need hardware/OS access they may not have: Windows/macOS/Android builds, encrypted media + physical drive checks (carried from M5), read-ahead thread for drives, resume-point save states, GL path, buildbot/CI (repo is local-only), docs.
- Launch command for the user (Steam RetroArch needs its runtime wrapper):
  `cd ~/.local/share/Steam/steamapps/common/RetroArch && ~/.local/share/Steam/steamapps/common/SteamLinuxRuntime/steam-runtime/run.sh ./retroarch.sh -L /home/deck/.claude/projects/DeeVee/deevee_libretro.so <iso|chd>`
- Build: `distrobox enter deevee-build -- make -j8` ; tests: `distrobox enter deevee-build -- make check`.

## 13. Decision log
 
| ID | Decision | Rationale | Status |
|---|---|---|---|
| D-001 | Use libdvdnav for all navigation; never re-implement the VM. | Mature, used by VLC/mpv/FFmpeg/GStreamer. | Accepted |
| D-002 | Own PS/PES demux + own SPU decoder; FFmpeg for A/V codecs only. | Event-accurate timing and cheap highlight updates (§3). | Accepted |
| D-003 | libdvdcss linkage, phased: **(1) M0–M6: static link** (built in, default). **(2) M7: distributed builds switch to libdvdread dlopen** of a user-provided libdvdcss. Controlled by `DEEVEE_CSS=static\|dlopen\|none` (§6); core code never calls libdvdcss directly. | Built-in removes setup friction while developing; dlopen keeps the distributed core free of CSS code. The switch makes phase 2 a build-flag change, not a refactor. | **Accepted (2026-10-15)** |
| D-004 | Core license GPL-2.0-or-later. | Compatible with all deps and VLC ports. | Accepted |
| D-005 | Single-threaded pull model first; threads only for device read-ahead. | Simplicity, portability, menu latency. | Accepted |
| D-006 | Software XRGB8888 output first; GL path later. | Fastest to correct; SD is cheap. | Accepted |
| D-007 | Pin FFmpeg **n8.1.3**, not the newest n9.0.2. | n9.0 is a fresh major with API removals; 8.1.x is the mature stable line and has every API we use. Revisit at M7 (§11). | Accepted (2026-10-15), confirmed by user |
| D-008 | `DEEVEE_CSS=none` builds libdvdread identically to `dlopen` (`-Dlibdvdcss=disabled`); only the reported mode differs. | libdvdread has no "never load CSS" switch; it always attempts `dlopen` when not linked. `none` = "no CSS code in the binary"; CI must run where libdvdcss is absent. | Accepted (2026-10-15) |
| D-009 | Toolchain on the Steam Deck dev box: distrobox `deevee-build` (Ubuntu 24.04). Core links only against libc, so the container-built `.so` runs in host RetroArch. | SteamOS root is read-only; container leaves host untouched. | Accepted (2026-10-15) |
| D-010 | On `HOP_CHANNEL` re-anchor the clock but **don't** flush decoders and **keep** VOBU continuity (VLC behaviour). Hard flush (`dv_video_flush`) is for user seeks only (M6). §5.2 table said "flush all". | Flushing drops the tail of every looping menu; resetting continuity made post-jump PTS land behind queued frames and caused false late-drops. | Accepted (2026-10-15) |
| D-011 | Block reading is gated on the furthest demuxed **audio/video** PTS (≤ clock + 1.5 s), not only the video queue; WAIT release and still timers wait until the clock has presented everything demuxed (VLC `ES_OUT_GET_EMPTY` equivalent). | Still-picture menus with minutes of audio (TIMERIC VMGM) otherwise get read in a few ticks and loop at high speed. | Superseded in part by D-012 (read gating); drain-before-WAIT/still still applies |
| D-012 | Video is buffered **compressed** (parsed pictures in a FIFO, ≤512) and decoded on demand; reading is gated per stream: continue until both the video and selected-audio demux horizons reach clock + 0.3 s, never beyond clock + 3 s. Replaces D-011's single horizon. | DVD muxes video ~0.5–1 s ahead of audio with the same PTS; gating on video alone delivered all audio late. A second of decoded SD frames is ~15 MB, compressed ~1 MB. | Accepted (2026-10-15) |
| D-013 | Audio is a queue of PTS-stamped chunks rendered per tick for `[clock, clock+tick)`. Blocks within 5 ms of the queue end join it; larger deviations must be **confirmed by the next timestamped block** before a new segment starts (jumps ≥ 1 s act at once); the first block is then re-placed at its own PTS. Playback (re)starts and segment starts align sample-exactly; continuous play tolerates 20 ms. 64-sample fades at segment edges. | Honours real gaps/overlaps at cell joins (DVD allows audio gaps at seamless joins) for lip sync; absorbs libavcodec parser PTS mis-association (DTS: same PTS on two consecutive frames) without audible breaks. | Accepted (2026-10-15) |
| D-014 | AC-3 and DTS downmix inside the decoder (`downmix=stereo`, honours the stream's centre/surround mix levels); swresample for everything else with `rematrix_maxval=1` (no clipping). AC-3 `drc_scale` left at FFmpeg's default 1.0 (line mode) until the M6 core option. | Most faithful stereo fold-down; never clips. Result is conservative in level (IDIOCRACY feature: peak −12 dBFS, RMS −39 dBFS). | Accepted (2026-10-15) |
| D-015 | Core options v2 started in M3 (Subtitles: Disc default/On/Off(forced only); Subtitle Language), applied once per title (`dv_nav_apply_subtitle_options`) so later user choices aren't overridden. §5.10's 'forced-only' merged into 'Off' since forced subtitles always show. | Only way to turn subtitles on before M4 input; needed for M3's 'selection by language'. | Accepted (2026-10-15) |
| D-016 | One subpicture per SPU packet (FFmpeg model): start = first STA/FSTA date, stop = STP date or 'until replaced'; colours/alpha/area: last value wins. CHG_COLCON skipped (M8), counted as unsupported. Overlong RLE runs clamped (VLC-tolerant) rather than rejected (FFmpeg). | Simple and matches both oracles on test content; menu SPUs with palette changes per DCSQ are rare. | Accepted (2026-10-15) |
| D-017 | Highlight rendering follows the DVD model (§5.5): highlight palette inside the button rectangle (inclusive), the subpicture's own palette elsewhere. VLC 3 instead crops the menu SPU to the rectangle (`vout_subpictures.c` force_crop, width ex−sx) and patches all-transparent palettes. Highlights also apply to buttons in title domains (VLC: menus only). | Spec-correct; identical to VLC on discs whose menu SPU base alpha is 0 (both Tim & Eric discs). | Accepted (2026-10-15) |
| D-018 | PCIs are queued with their timeline start; highlight, D-pad, activation and pointer use the PCI on screen, not `dvdnav_get_current_nav_pci()` (which is up to 3 s ahead after D-012). User-initiated jumps (activate, menu calls, Return, chapter skip, pointer activate, auto-action) hard-flush video/audio/SPU/PCI queues and clock horizons; disc-initiated hops keep D-010. | Buttons go live when they appear on screen; menu response is immediate instead of after 3 s of queued old content. | Accepted (2026-10-15) |
| D-019 | Drop the Parental Level core option from §5.10. | libdvdnav has no API to set SPRM 13; changing it means reaching into private VM state (against D-001). Unrestricted (15) is the right default for a player of one's own discs. | Accepted (2026-10-15) |
| D-020 | CHD support via libchdr (BSD-3) compiled into the core; CHD is another `dv_stream_t` backend (one-hunk cache) behind the same libdvdread stream callbacks, raw file I/O still through the libretro VFS. Only DVD CHDs (`DVD ` metadata) accepted. | User request; zero impact on the rest of the pipeline; CSS keeps working (libdvdcss stream mode). | Accepted (2026-10-15) |
| D-021 | Deinterlace default: **bwdif at field rate** with `deint=interlaced` (only pictures flagged interlaced; film/progressive frames and pulldown neighbours pass through once). Filter sits after decode, before the presentation queue; PTS interpolated before filtering; drained at forced stills. | §5.4 said 'off for progressive, simple blend otherwise'; field rate gives 59.94 distinct pictures from 29.97i video at no visible cost on the Deck and leaves film cadence untouched. | Accepted (2026-10-15) |
| D-022 | Title-mode input: Select = info OSD (§5.9 'toggle OSD'); L2/R2 = hold-to-scan by repeated ±15 s VOBU-accurate jumps every 300 ms (not time-stretched playback); A = pause. User-initiated jumps unpause. | Sector-accurate, shows real pictures while scanning, no audio artefacts. | Accepted (2026-10-15) |
| D-023 | 'Letterbox 16:9 on a 4:3 display' aspect mode deferred (needs a letterboxed render path + letterbox SPU variant via SPU_STREAM_CHANGE physical_letterbox). Aspect option offers Auto/4:3/16:9. | Frontends scale to any display; low value vs. effort now. | Superseded by D-025 (2026-10-16) |
| D-024 | libdvdcss is user-supplied and lives in RetroArch's **system directory**, handled like a BIOS (optional firmware entry in the .info; core pre-loads it by absolute path before libdvdread's by-name dlopen). | Matches how RetroArch users already supply BIOS files; keeps distributed cores free of CSS code; works the same on every desktop platform. | Accepted (2026-10-16) |
| D-026 | libdvdnav carries a local patch (`patches/libdvdnav/*.patch`, applied to a copy under build/ by Makefile.deps; submodule stays pristine): `dvdnav_set_player_video_config()` fixes SPRM 14 (otherwise libdvdnav rewrites its TV-aspect bits to the source aspect on every query) and `dvdnav_get_active_spu_stream_mode()`. | No public API exists; both are tiny and upstreamable. | Accepted (2026-10-16) |
| D-027 | 4:3 TV modes honour the IFO's permitted display modes (P&S-only / LB-only override the user's choice), like hardware players. Menus on both 16:9 test discs are P&S-only, features LB-only. Overlay space: if the selected SPU for the mode is a distinct stream it's authored for the 4:3 frame (compose after transform, matching button group); otherwise compose on the stored picture and transform both. | Matches player behaviour and the disc's authored SPU/button variants. | Accepted (2026-10-16) |
| D-025 | Add a 'TV Display Type' option (16:9 / 4:3 Letterbox / 4:3 Pan & Scan) mirroring hardware players: sets SPRM 14, picks SPU variant, renders letterbox/pan-scan. Implemented as M6b before M7. | User request; also makes discs with display-mode-specific menus behave correctly. | Accepted (2026-10-16) |
 
---
 
## 14. Key references
- libdvdnav / libdvdread: https://code.videolan.org/videolan/libdvdnav , https://code.videolan.org/videolan/libdvdread
- libdvdcss: https://code.videolan.org/videolan/libdvdcss
- VLC source: https://code.videolan.org/videolan/vlc
- FFmpeg source: https://git.ffmpeg.org/ffmpeg.git (dvdvideo demuxer docs: https://ffmpeg.org/ffmpeg-formats.html#dvdvideo)
- libretro API: https://github.com/libretro/libretro-common/blob/master/include/libretro.h ; docs: https://docs.libretro.com/development/cores/developing-cores/
- Prior art: RetroArch FFmpeg core (RetroArch/cores/libretro-ffmpeg), libretro-mpv (https://github.com/libretro/libretro-mpv)
- DVD format background: http://dvd.sourceforge.net/dvdinfo/ (IFO, PCI/DSI, SPU format specs)
