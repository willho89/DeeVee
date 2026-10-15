#!/usr/bin/env bash
# DeeVee — build unencrypted DVD-Video test fixtures (CLAUDE.md §8).
# Requires: ffmpeg (with ac3 encoder), dvdauthor, genisoimage.
#
#   make_fixture.sh <out_dir>
#
# Produces, each as <name>/VIDEO_TS and <name>.iso:
#   ntsc43  — First Play -> title 1: NTSC 720x480 4:3, 29.97, 2 chapters, AC-3
#   pal169  — First Play -> title 1: PAL 720x576 16:9 anamorphic, 25, 2 chapters, AC-3
set -euo pipefail
OUT=${1:?usage: make_fixture.sh <out_dir>}
mkdir -p "$OUT"
WORK=$(mktemp -d "$OUT/.work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# make_disc <name> <ntsc|pal> <4:3|16:9> <size> <rate> <volid>
make_disc() {
  local name=$1 fmt=$2 aspect=$3 size=$4 rate=$5 volid=$6
  export VIDEO_FORMAT=${fmt^^}

  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=${size}:rate=${rate}" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -t 12 -target "${fmt}-dvd" -aspect "$aspect" "$WORK/$name.mpg"

  cat > "$WORK/$name.xml" <<XML
<dvdauthor dest="$OUT/$name">
  <vmgm>
    <fpc>jump title 1;</fpc>
  </vmgm>
  <titleset>
    <titles>
      <video format="$fmt" aspect="$aspect"/>
      <audio format="ac3" lang="en"/>
      <pgc>
        <vob file="$WORK/$name.mpg" chapters="0,0:06"/>
        <post>exit;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
XML

  rm -rf "${OUT:?}/$name"
  dvdauthor -x "$WORK/$name.xml" >/dev/null 2>&1
  genisoimage -quiet -dvd-video -V "$volid" -o "$OUT/$name.iso" "$OUT/$name"
  echo "fixture: $OUT/$name.iso"
}

make_disc ntsc43 ntsc 4:3  720x480 30000/1001 DEEVEE_NTSC43
make_disc pal169 pal  16:9 720x576 25         DEEVEE_PAL169
