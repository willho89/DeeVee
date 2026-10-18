#!/usr/bin/env python3
# DeeVee test fixture: copy a DVD image with a mid-title NAV pack and the
# sector after it zero-filled, as ddrescue leaves unreadable sectors of a
# rotted disc. libdvdnav then reports "Expected NAV packet but none found".
#   damage.py <in.iso> <out.iso>
import sys

src, dst = sys.argv[1:3]
data = bytearray(open(src, "rb").read())
navs = [off for off in range(0, len(data), 2048)
        if data[off:off + 4] == b"\x00\x00\x01\xba"
        and data[off + 14:off + 18] == b"\x00\x00\x01\xbb"]
off = navs[len(navs) // 2]
data[off:off + 4096] = bytes(4096)
print("zeroed LBA %d-%d (NAV pack %d of %d)" % (off // 2048, off // 2048 + 1, len(navs) // 2, len(navs)))
open(dst, "wb").write(data)
