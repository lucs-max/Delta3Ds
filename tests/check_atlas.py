#!/usr/bin/env python3
"""Checks that a converted atlas decodes back to the test sprite's pixels."""
import struct
import sys

import numpy as np

d = sys.argv[1]
idx = open(d + "/atlas.idx", "rb").read()
assert idx[:4] == b"D3AI", "bad index magic"
version, nitems, natlas = struct.unpack_from("<III", idx, 4)
items = [struct.unpack_from("<hHHHHHH", idx, 16 + i * 14) for i in range(nitems)]
raw = open(d + "/atlas_000.bin", "rb").read()
assert raw[:4] == b"D3TX", "bad atlas magic"
w, h, fmt = struct.unpack_from("<HHI", raw, 4)
px = np.frombuffer(raw[12:], dtype=np.uint8).reshape(-1, 4)
ys, xs = np.mgrid[0:h, 0:w]
dst = (((ys >> 3) * (w >> 3) + (xs >> 3)) * 64 + ((xs & 1) | ((ys & 1) << 1) | ((xs & 2) << 1) |
                                                   ((ys & 2) << 2) | ((xs & 4) << 2) | ((ys & 4) << 3))).reshape(-1)
img = px[dst].reshape(h, w, 4)[::-1][:, :, [3, 2, 1, 0]]  # ABGR -> RGBA, top-down
expected = [(255, 0, 0, 255), (0, 0, 255, 255)]
for (atlas, x, y, iw, ih, ow, oh), want in zip(items, expected):
    region = img[y:y + ih, x:x + iw]
    assert (region == np.array(want, dtype=np.uint8)).all(), "item pixels differ"
print("atlas ok")
