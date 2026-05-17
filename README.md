# DeeVee

DeeVee is a new libretro core intended to emulate a DVD-Video player. The goal is to load DVD `.iso` images, `.chd` images, and raw DVD folder layouts, then support DVD menus, extras, navigation, video playback, subtitles, and audio through standard libretro frontends.

This repository currently contains the first scaffold only. It is buildable, validates DVD-like content paths, reserves joypad inputs for DVD menu navigation, renders a deterministic XRGB8888 placeholder frame, and emits silent 48 kHz stereo audio.

`include/libretro.h` is vendored from upstream `libretro-common` so the scaffold can build without a system libretro install.

## Build

```sh
make
```

On Windows, run this from an environment with a C compiler on `PATH`, such as MSYS2/MinGW GCC, LLVM/Clang, or a configured Visual Studio developer shell.

The default target builds a platform-specific libretro shared library:

- Windows: `deevee_libretro.dll`
- Linux and other Unix-like systems: `deevee_libretro.so`
- macOS: `deevee_libretro.dylib`

To run the lightweight content detection tests:

```sh
make test
```

## Current Content Handling

`retro_load_game` requires a full path and currently accepts:

- `.iso` files
- `.chd` files
- `.ifo` files
- DVD folders containing either `VIDEO_TS/VIDEO_TS.IFO` or `VIDEO_TS.IFO`

The scaffold does not decode DVD sectors, MPEG-2 video, subpictures, or audio yet.

## Controls Reserved For DVD Navigation

- D-pad: menu navigation
- A: confirm/select
- B: cancel/back
- Start or X: menu

## Near-Term Roadmap

1. Replace placeholder content probing with real ISO/UDF and DVD folder opening.
2. Add CHD-backed sector reading through `libchdr`.
3. Integrate DVD navigation and VM behavior, likely through a `libdvdnav`-style boundary.
4. Decode MPEG-2 video and subpicture overlays into the XRGB8888 frame buffer.
5. Decode or passthrough DVD audio formats, starting with AC3/LPCM handling.
6. Add frontend-visible options for aspect ratio, deinterlacing, region behavior, and menu defaults.

## License

DeeVee is licensed under the GNU General Public License version 3. See `LICENSE`.
