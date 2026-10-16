#!/usr/bin/env python3
# DeeVee test fixture: copy a DVD image with the PES_scrambling_control bits
# set on every audio/video pack, as an undecrypted CSS disc reads without
# libdvdcss. The payload stays clear; only the flags matter for the test.
#   scramble.py <in.iso> <out.iso>
import sys

src, dst = sys.argv[1:3]
data = bytearray(open(src, "rb").read())
for off in range(0, len(data), 2048):
    s = data[off:off + 24]
    if (s[:4] == b"\x00\x00\x01\xba" and s[14:17] == b"\x00\x00\x01"
            and s[17] in (0xE0, 0xBD, 0xC0) and (s[20] & 0xC0) == 0x80):
        data[off + 0x14] |= 0x10
open(dst, "wb").write(data)
