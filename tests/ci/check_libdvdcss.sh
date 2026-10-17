#!/bin/sh
# DeeVee CI: libdvdcss from RetroArch's system folder (D-024).
# Builds libdvdcss as a shared library, then plays the scrambled fixture
# with an empty system folder (must ask for the library by name) and with
# the library in it (must load it from there and hand it to libdvdread).
#
#   check_libdvdcss.sh <core> <harness> <libdvdcss file name>
set -eu
core=$1 harness=$2 libname=$3
work=build/ci-libdvdcss
rm -rf "$work"
mkdir -p "$work/empty" "$work/with"
meson setup "$work/build" deps/libdvdcss --buildtype=release --default-library=shared \
  -Denable_docs=false >/dev/null
ninja -C "$work/build" >/dev/null
lib=$(find "$work/build/src" -name "$libname" | head -1)
[ -n "$lib" ] || { echo "no $libname built"; ls "$work/build/src"; exit 1; }
cp -L "$lib" "$work/with/$libname"

run() { HARNESS_DEBUG=1 "$harness" "$core" build/fixtures/scrambled.iso 120 "$1" 2>&1 | tr -d '\r'; }

out=$(run "$work/empty")
echo "$out" | grep -q "OSD: This disc is encrypted: place $libname" \
  || { echo "$out" | grep -E 'libdvdcss|OSD'; echo "FAIL: no 'place $libname' message"; exit 1; }
echo "   no libdvdcss: asks for $libname (ok)"

out=$(run "$work/with")
echo "$out" | grep -q "libdvdcss: loaded" \
  || { echo "$out" | grep -E 'libdvdcss|OSD'; echo "FAIL: $libname not loaded from the system folder"; exit 1; }
if echo "$out" | grep -q "Encrypted DVD support unavailable"; then
  echo "$out" | grep -E 'libdvdcss|Encrypted|OSD'
  echo "FAIL: loaded, but libdvdread didn't pick it up"
  exit 1
fi
echo "   libdvdcss in the system folder: loaded and used by libdvdread (ok)"
