# DeeVee

A DVD-Video player core for [RetroArch](https://www.retroarch.com/) (libretro).

DeeVee plays DVD images with full disc menus: button highlights, still and motion menus, sub-menus and special features behave the way they do on a set-top player. It's region-free, and it renders subpictures (subtitles and menu overlays) itself, so menu highlights land exactly where the disc puts them.

It's built on [libdvdnav](https://code.videolan.org/videolan/libdvdnav) / [libdvdread](https://code.videolan.org/videolan/libdvdread) for navigation and [FFmpeg](https://ffmpeg.org/) for decoding. Large parts of the glue are ported from VLC.

> **Status: alpha.** Plays real discs end to end on Linux (tested on a Steam Deck). Builds for other platforms are in progress, see [Status](#status).

## Features

- **Content:** `.iso` images, `.chd` DVD images (from `chdman createdvd`), and `VIDEO_TS` folders (load `VIDEO_TS.IFO`).
- **Menus:** the disc's own virtual machine runs as written (First Play, root/title/chapter/audio/subtitle menus, timed and infinite stills, motion menus with audio, auto-action buttons). Control them with the RetroPad, a mouse or touch.
- **Any region:** the player's region is chosen from the disc, which also passes region checks (RCE) inside the disc's menus. You can force a region instead.
- **Video:** MPEG-1/2, NTSC and PAL, 4:3 and anamorphic 16:9, 3:2 pulldown, deinterlacing (bwdif/yadif, field rate).
- **TV Display Type:** 16:9, 4:3 Letterbox or 4:3 Pan & Scan, like a hardware player's setup menu. Discs with separate 4:3 menus or subtitles use them, and the disc's permitted display modes are respected.
- **Audio:** AC-3, DTS, LPCM and MPEG audio, downmixed to stereo or Dolby Pro Logic II, with optional dynamic range compression (night mode).
- **Subtitles:** on, off or disc default, preferred language, and forced subtitles always shown.
- **Controls during playback:** seek, pause, chapter skip, hold-to-scan, audio/subtitle/angle cycling and an info OSD.
- **Encrypted discs:** optional. Decrypted rips (e.g. MakeMKV ISOs) need nothing extra. CSS-encrypted images need `libdvdcss` in RetroArch's system folder (see below).

## Installing

1. Copy the core for your platform (`deevee_libretro.so` / `.dll` / `.dylib`) into RetroArch's `cores` folder and `deevee_libretro.info` into its `info` folder.
2. In RetroArch: **Load Core → DeeVee**, then **Load Content** and pick an `.iso`, `.chd` or `VIDEO_TS.IFO`.

### Encrypted discs (optional)

DeeVee doesn't include CSS decryption. If a disc image is still CSS-encrypted, put **libdvdcss** in RetroArch's **system** folder, the same place BIOS files go:

| Platform | File name |
|---|---|
| Linux / Android | `libdvdcss.so.2` |
| Windows | `libdvdcss-2.dll` |
| macOS | `libdvdcss.2.dylib` |

On Linux an installed system libdvdcss (e.g. the `libdvdcss2` package) is also picked up. Without it, unencrypted images play normally. An encrypted one stops with an on-screen message naming the file to install.

## Controls

| RetroPad | In a menu | During playback |
|---|---|---|
| D-pad | Move between buttons | Left/Right: seek ±10 s · Up/Down: ±60 s |
| A | Activate button / continue a still | Pause / resume |
| B | Return (go up a menu) | Return |
| Start | Root menu | Root menu |
| Select | Title menu | Info (title, chapter, time, tracks) |
| L / R | — | Previous / next chapter |
| L2 / R2 (hold) | — | Scan backward / forward |
| X / Y | — | Cycle audio / subtitles |
| L3 | — | Cycle angle |

Mouse and touch: hover selects a menu button and click/tap activates it.

## Core options

| Category | Options |
|---|---|
| Video | Deinterlace · Aspect Ratio (auto / 4:3 / 16:9 override) · TV Display Type |
| Audio | Downmix (stereo / Pro Logic II) · Dynamic Range Compression · Audio Language |
| Subtitles | Subtitles (disc default / on / off) · Subtitle Language |
| Disc & Region | Player Region · Menu Language · CSS Decryption Method · End of Disc (restart / stop / close) · Show OSD |

## Status

| Area | State |
|---|---|
| Playback, A/V sync, subtitles, interactive menus | ✅ Done; menus checked against VLC button by button (TIMERIC: 14 menus, ~100 paths, 3 bugs found and fixed) |
| Region / languages / CHD | ✅ Done |
| Seek, pause, chapters, tracks, deinterlace, OSD | ✅ Done |
| TV Display Type (16:9 / letterbox / pan & scan) | ✅ Done |
| libdvdcss from the system folder | ✅ Linux, Windows (tested under Wine) · ⏳ macOS, Android |
| Linux x86_64 build | ✅ |
| Windows x86_64 build | ✅ Cross-compiled; full test suite passes under Wine, real-Windows check pending |
| macOS, Android, ARM Linux builds | ⏳ Planned |
| Save states (resume points: position + the disc's own memory) | ✅ Done |
| RetroArch "Restart" (starts the disc over) | ✅ Done |
| README / docs page for docs.libretro.com | ⏳ This README; docs page planned |
| Closed captions, `CHG_COLCON`, DVD-VR, karaoke | 💤 Later |

**Not yet verified:** CSS decryption on a genuinely encrypted image, real PAL discs, multi-angle discs, and discs with per-TV-type menus. Reports are welcome.

**Out of scope:** physical DVD drives, Blu-ray, DVD-Audio.

This is a from-scratch rewrite; it replaces an earlier, abandoned attempt that lived in this repository.

## Building

Linux (the reference platform). You need a C toolchain, `make`, Meson, Ninja, pkg-config and NASM.

```sh
git clone --recursive https://github.com/willho89/DeeVee.git
cd DeeVee
make -j8                     # libdvdcss built in (local/dev builds)
make -j8 DEEVEE_CSS=dlopen   # distribution build: libdvdcss loaded from the system folder
make check                   # headless test suite on generated fixture discs
```

Windows (cross-compiled from Linux with MinGW-w64; tests run under Wine):

```sh
make -j8 platform=win DEEVEE_CSS=dlopen   # -> deevee_libretro.dll
make platform=win check
```

The first `make` builds the dependencies (libdvdcss, libdvdread, libdvdnav, a minimal FFmpeg) as static libraries under `build/`. The core links only against libc. libdvdnav gets a small local patch from `patches/libdvdnav/`.

`make check-discs` runs extra menu regression checks against your own discs in `TestISOs/` (gitignored, never committed). `tests/oracle/` has the tools used to compare DeeVee's menu navigation with VLC's.

Development notes, the full plan and the decision log are in [`CLAUDE.md`](CLAUDE.md).

## License

GPL-2.0-or-later. Bundled dependencies keep their own licenses: libdvdnav/libdvdread/libdvdcss GPL-2.0+, FFmpeg LGPL-2.1+, libchdr BSD-3-Clause, libretro-common MIT. Code ported from VLC keeps its original copyright notices.
