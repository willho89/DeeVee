#!/usr/bin/env bash
# DeeVee — build an unencrypted DVD-Video test fixture (CLAUDE.md §8).
# Requires: ffmpeg (with ac3 encoder), dvdauthor, genisoimage.
#
#   make_fixture.sh <out_dir>      -> <out_dir>/ntsc43/VIDEO_TS, <out_dir>/ntsc43.iso
#
# Content: VMG First Play -> title 1 (NTSC 720x480 4:3, 2 chapters, AC-3 tone).
set -euo pipefail
OUT=${1:?usage: make_fixture.sh <out_dir>}
mkdir -p "$OUT"
WORK=$(mktemp -d "$OUT/.work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

export VIDEO_FORMAT=NTSC

ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=720x480:rate=30000/1001" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000" \
  -t 12 -target ntsc-dvd -aspect 4:3 "$WORK/title.mpg"

cat > "$WORK/dvd.xml" <<XML
<dvdauthor dest="$OUT/ntsc43">
  <vmgm>
    <fpc>jump title 1;</fpc>
  </vmgm>
  <titleset>
    <titles>
      <video format="ntsc" aspect="4:3"/>
      <audio format="ac3" lang="en"/>
      <pgc>
        <vob file="$WORK/title.mpg" chapters="0,0:06"/>
        <post>exit;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
XML

rm -rf "$OUT/ntsc43"
dvdauthor -x "$WORK/dvd.xml" >/dev/null 2>&1
genisoimage -quiet -dvd-video -V DEEVEE_NTSC43 -o "$OUT/ntsc43.iso" "$OUT/ntsc43"
echo "fixture: $OUT/ntsc43.iso and $OUT/ntsc43/VIDEO_TS"
