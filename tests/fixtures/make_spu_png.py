#!/usr/bin/env python3
# DeeVee — write a truecolour PNG with one bordered box on a key colour, for
# spumux test subpictures (<= 4 colours: key, fill, border, unused).
#   make_spu_png.py out.png W H x0 y0 x1 y1 fill_rrggbb border_rrggbb
import struct, sys, zlib

def rgb(h):
    return bytes.fromhex(h)

out, W, H, x0, y0, x1, y1, fill, border = sys.argv[1], *map(int, sys.argv[2:8]), rgb(sys.argv[8]), rgb(sys.argv[9])
key = rgb("010101")   # transparent in spumux (transparent="010101")
rows = []
for y in range(H):
    r = bytearray(b"\x00")
    for x in range(W):
        if x0 <= x <= x1 and y0 <= y <= y1:
            edge = x - x0 < 4 or x1 - x < 4 or y - y0 < 4 or y1 - y < 4
            r += border if edge else fill
        else:
            r += key
    rows.append(bytes(r))
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
