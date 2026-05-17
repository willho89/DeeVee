# DeeVee

DeeVee is an experimental libretro core for DVD-Video playback. The goal is to load DVD `.iso` and `.chd` images in RetroArch and eventually support DVD menus, extras, navigation, video playback, subtitles, and audio through standard libretro frontends.

Current version: `0.4.0-alpha`.

This is not a feature-complete DVD player yet. It is an early playable milestone with real DVD sector access, MPEG-2 video decoding, AC3 audio decoding, basic menu button navigation, CHD support, and targeted submenu handling for the primary test disc. Many DVD VM commands, menu flows, subtitles, and compatibility cases are still incomplete.

`include/libretro.h` is vendored from upstream `libretro-common` so the scaffold can build without a system libretro install.

## Current Status

Implemented:

- Libretro core entry points for RetroArch.
- DVD ISO loading and ISO9660 path lookup.
- CHD-backed DVD image loading through vendored `libchdr`.
- IFO parsing for VMG/VTS metadata, title tables, menu PGCs, NAV packets, and button metadata.
- MPEG Program Stream packet walking for VOB content.
- MPEG-2 video decoding through FFmpeg.
- AC3 audio decoding through FFmpeg, including stereo downmixing.
- Basic frame pacing using DVD stream timing information.
- Keyboard and RetroPad directional menu navigation.
- Basic visual menu button highlight overlay.
- Targeted DVD VM command handling for observed title jumps and some menu/submenu transitions.
- `tools/deevee_probe` diagnostics for content type, DVD structures, packets, menu commands, video, and audio.

Known limitations:

- DVD VM support is partial. Many commands involving registers, conditions, resume behavior, menu domains, and more complex navigation are not implemented yet.
- Submenu navigation is still spotty. Some Setup/Special Features paths work or partially work on the main test disc, but broader compatibility is not expected yet.
- Subtitle/subpicture rendering is not implemented.
- Menu highlights are diagnostic/basic and are not full DVD subpicture compositing.
- Playback compatibility has mostly been tested against a small set of DVD images with wider testing to come once feature complete on initial test sample.
- No frontend options exist yet for region behavior, deinterlacing, aspect overrides, language defaults, or subtitle/audio stream selection.

## Releases

The first public checkpoint is `v0.4.0-alpha`.

For Windows RetroArch, download the release ZIP and copy:

- `cores/deevee_libretro.dll` to your RetroArch `cores` folder.
- `info/deevee_libretro.info` to your RetroArch `info` folder.

## Build

```sh
make
```

On Windows, run this from an environment with a C compiler on `PATH`, such as MSYS2/MinGW GCC, LLVM/Clang, or a configured Visual Studio developer shell.

Feature flags:

- `HAVE_FFMPEG=1` enables MPEG-2 video and AC3 audio decoding.
- `HAVE_CHD=1` enables CHD image support through the vendored `deps/libchdr` sources.

Typical Windows development build:

```sh
make HAVE_FFMPEG=1 HAVE_CHD=1
```

The default target builds a platform-specific libretro shared library:

- Windows: `deevee_libretro.dll`
- Linux and other Unix-like systems: `deevee_libretro.so`
- macOS: `deevee_libretro.dylib`

To run the lightweight content detection tests:

```sh
make test
```

To build and run the host-side content probe:

```sh
make HAVE_FFMPEG=1 HAVE_CHD=1 probe
./tools/deevee_probe /path/to/disc.iso /path/to/disc.chd
```

## Current Content Handling

`retro_load_game` requires a full path and currently accepts:

- `.iso` files
- `.chd` files

DVD folder loading is not supported yet.

## Controls

- D-pad: menu navigation
- A or keyboard `X`: confirm/select
- B: cancel/back
- Start or X: menu
- Keyboard arrow keys: menu navigation
- Keyboard `Enter`: confirm/select

## Roadmap

Near-term work:

1. Broaden DVD VM command support so menu navigation works across more discs.
2. Implement proper subpicture/subtitle decoding and compositing.
3. Improve menu highlight rendering to use DVD palette/subpicture data.
4. Add audio/subtitle/language stream selection.
5. Add frontend-visible options for aspect ratio, deinterlacing, region behavior, and menu defaults.
6. Expand compatibility testing beyond the current small test set.

## License

DeeVee is licensed under the GNU General Public License version 3. See `LICENSE`.
