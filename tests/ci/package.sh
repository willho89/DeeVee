#!/bin/sh
# DeeVee release packaging: strip one core and zip it with the core info
# file, README, licence and the bundled libraries' licences.
#
#   package.sh <core> <strip command> <platform name> <version>
set -eu
core=$1 strip=$2 platform=$3 version=$4
name=deevee-$version-$platform
out=build/package/$name
rm -rf "$out"
mkdir -p "$out/licenses"
cp "$core" "$out/"
$strip "$out/$(basename "$core")"
cp deevee_libretro.info README.md LICENSE "$out/"
cp deps/libdvdnav/COPYING  "$out/licenses/libdvdnav-COPYING.txt"
cp deps/libdvdread/COPYING "$out/licenses/libdvdread-COPYING.txt"
cp deps/ffmpeg/COPYING.LGPLv2.1 "$out/licenses/ffmpeg-LGPL-2.1.txt"
cp deps/libchdr/LICENSE.txt "$out/licenses/libchdr-LICENSE.txt"
cat > "$out/INSTALL.txt" <<TXT
DeeVee $version ($platform) - DVD-Video player core for RetroArch

1. Copy $(basename "$core") into RetroArch's "cores" folder
   and deevee_libretro.info into its "info" folder.
   (Android: Load Core -> Install or Restore a Core, pick $(basename "$core").)
2. Load Core -> DeeVee, then Load Content -> an .iso, .chd or VIDEO_TS.IFO.

Decrypted rips (e.g. MakeMKV ISOs) play as they are. CSS-encrypted images
need libdvdcss in RetroArch's "system" folder (libdvdcss.so.2 on Linux and
Android, libdvdcss-2.dll on Windows, libdvdcss.2.dylib on macOS); it is not
included. See README.md.

Source code (GPL-2.0-or-later), including all bundled libraries:
https://github.com/willho89/DeeVee/releases/tag/v$version
TXT
(cd build/package && rm -f "$name.zip" && zip -qr "$name.zip" "$name")
echo "build/package/$name.zip"
