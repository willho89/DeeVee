#!/usr/bin/env bash
# DeeVee — build unencrypted DVD-Video test fixtures (CLAUDE.md §8).
# Requires: ffmpeg (with ac3 encoder), dvdauthor, genisoimage.
#
#   make_fixture.sh <out_dir>
#
# Produces, each as <name>/VIDEO_TS and <name>.iso:
#   ntsc43  — First Play -> title 1: NTSC 720x480 4:3, 29.97, 2 chapters, AC-3
#   pal169  — First Play -> title 1: PAL 720x576 16:9 anamorphic, 25, 2 chapters, AC-3
#   avs_ac3, avs_lpcm, avs_mp2, avs_dts — A/V sync: NTSC 4:3, a 2-frame white
#            flash and a 100 ms 1 kHz beep every 30 frames (1.001 s, frame-aligned), as two 10 s
#            VOB files (= two cells, PTS restart at the boundary).
#            avs_ac3 is 5.1 with the beep only in the centre channel.
#   subs    — subpictures: NTSC 4:3, 10 s dark grey; subtitles OFF by default.
#            stream 0 "en": white box (bottom)       1-3 s
#                           green box (bottom-left)  4-6 s
#                           yellow box (top) FORCED  7-9 s
#            stream 1 "fr": magenta box (middle)     1-9 s
#   region2 — region-2-only disc (VMG category patched) with an RCE-style
#            check: First Play jumps to title 1 (GREEN) if SPRM20 == 2,
#            otherwise title 2 (RED, "wrong region").
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

# make_avsync <name> <dvdauthor audio format> <extra audio filter> <ffmpeg audio args...>
make_avsync() {
  local name=$1 afmt=$2 afilt=$3; shift 3
  export VIDEO_FORMAT=NTSC
  local seg
  for seg in a b; do
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "color=c=black:s=720x480:r=30000/1001:d=10" \
      -f lavfi -i "color=c=white:s=720x480:r=30000/1001:d=10" \
      -f lavfi -i "sine=frequency=1000:sample_rate=48000:samples_per_frame=16:d=10" \
      -filter_complex "[0][1]overlay=enable='lt(mod(t\,1.001)\,0.066)'[v];[2]volume=volume=0:enable='gte(mod(t\,1.001)\,0.1)',$afilt[a]" \
      -map "[v]" -map "[a]" -target ntsc-dvd -aspect 4:3 "$@" "$WORK/${name}_$seg.mpg"
  done

  cat > "$WORK/$name.xml" <<XML
<dvdauthor dest="$OUT/$name">
  <vmgm>
    <fpc>jump title 1;</fpc>
  </vmgm>
  <titleset>
    <titles>
      <video format="ntsc" aspect="4:3"/>
      <audio format="$afmt" lang="en"/>
      <pgc>
        <vob file="$WORK/${name}_a.mpg"/>
        <vob file="$WORK/${name}_b.mpg"/>
        <post>exit;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
XML
  rm -rf "${OUT:?}/$name"
  dvdauthor -x "$WORK/$name.xml" >/dev/null 2>&1
  genisoimage -quiet -dvd-video -V "DEEVEE_${name^^}" -o "$OUT/$name.iso" "$OUT/$name"
  echo "fixture: $OUT/$name.iso"
}

make_disc ntsc43 ntsc 4:3  720x480 30000/1001 DEEVEE_NTSC43
make_disc pal169 pal  16:9 720x576 25         DEEVEE_PAL169

make_avsync avs_ac3  ac3 "pan=5.1|FC=c0" -c:a ac3 -ac 6 -b:a 448k
make_avsync avs_lpcm pcm anull -c:a pcm_s16be -ac 2
make_avsync avs_mp2  mp2 anull -c:a mp2 -ac 2 -b:a 224k
make_avsync avs_dts  dts anull -c:a dca -strict -2 -ac 2 -b:a 768k

# ---- subpicture fixture --------------------------------------------------
make_subs() {
  local name=subs png="$(dirname "$0")/make_spu_png.py"
  export VIDEO_FORMAT=NTSC
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=0x303030:s=720x480:r=30000/1001:d=10" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo:d=10" \
    -target ntsc-dvd -aspect 4:3 -shortest "$WORK/subs_v.mpg"

  python3 "$png" "$WORK/s_white.png"   720 480 260 400 459 439 ffffff 000000
  python3 "$png" "$WORK/s_green.png"   720 480  40 400 239 439 00c000 000000
  python3 "$png" "$WORK/s_yellow.png"  720 480 260  40 459  79 ffff00 000000
  python3 "$png" "$WORK/s_magenta.png" 720 480 300 220 419 259 ff00ff ffffff

  cat > "$WORK/s0.xml" <<XML
<subpictures><stream>
  <spu start="00:00:01.00" end="00:00:03.00" image="$WORK/s_white.png"  transparent="010101"/>
  <spu start="00:00:04.00" end="00:00:06.00" image="$WORK/s_green.png"  transparent="010101"/>
  <spu start="00:00:07.00" end="00:00:09.00" image="$WORK/s_yellow.png" transparent="010101" force="yes"/>
</stream></subpictures>
XML
  cat > "$WORK/s1.xml" <<XML
<subpictures><stream>
  <spu start="00:00:01.00" end="00:00:09.00" image="$WORK/s_magenta.png" transparent="010101"/>
</stream></subpictures>
XML
  spumux -s 0 "$WORK/s0.xml" < "$WORK/subs_v.mpg" > "$WORK/subs_s0.mpg" 2>/dev/null
  spumux -s 1 "$WORK/s1.xml" < "$WORK/subs_s0.mpg" > "$WORK/subs_s1.mpg" 2>/dev/null

  cat > "$WORK/$name.xml" <<XML
<dvdauthor dest="$OUT/$name">
  <vmgm>
    <fpc>jump title 1;</fpc>
  </vmgm>
  <titleset>
    <titles>
      <video format="ntsc" aspect="4:3"/>
      <audio format="ac3" lang="en"/>
      <subpicture lang="en"/>
      <subpicture lang="fr"/>
      <pgc>
        <vob file="$WORK/subs_s1.mpg"/>
        <post>exit;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
XML
  rm -rf "${OUT:?}/$name"
  dvdauthor -x "$WORK/$name.xml" >/dev/null 2>&1
  genisoimage -quiet -dvd-video -V DEEVEE_SUBS -o "$OUT/$name.iso" "$OUT/$name"
  echo "fixture: $OUT/$name.iso"
}
make_subs

# ---- region fixture -------------------------------------------------------
make_region2() {
  local name=region2
  export VIDEO_FORMAT=NTSC
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=green:s=720x480:r=30000/1001:d=4" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo:d=4" -target ntsc-dvd -aspect 4:3 -shortest "$WORK/r_ok.mpg"
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=red:s=720x480:r=30000/1001:d=4" \
    -f lavfi -i "anullsrc=r=48000:cl=stereo:d=4" -target ntsc-dvd -aspect 4:3 -shortest "$WORK/r_bad.mpg"
  cat > "$WORK/$name.xml" <<XML
<dvdauthor dest="$OUT/$name">
  <vmgm>
    <fpc>if (s20 == 2) jump title 1; jump title 2;</fpc>
  </vmgm>
  <titleset>
    <titles>
      <video format="ntsc" aspect="4:3"/>
      <audio format="ac3" lang="en"/>
      <pgc><vob file="$WORK/r_ok.mpg"/><post>exit;</post></pgc>
      <pgc><vob file="$WORK/r_bad.mpg"/><post>exit;</post></pgc>
    </titles>
  </titleset>
</dvdauthor>
XML
  rm -rf "${OUT:?}/$name"
  dvdauthor -x "$WORK/$name.xml" >/dev/null 2>&1
  # VMG category byte 0x23: bit n set = region n+1 NOT allowed. Region 2 only.
  for f in "$OUT/$name/VIDEO_TS/VIDEO_TS.IFO" "$OUT/$name/VIDEO_TS/VIDEO_TS.BUP"; do
    printf '\xfd' | dd of="$f" bs=1 seek=$((0x23)) conv=notrunc status=none
  done
  genisoimage -quiet -dvd-video -V DEEVEE_REGION2 -o "$OUT/$name.iso" "$OUT/$name"
  echo "fixture: $OUT/$name.iso"
}
make_region2
