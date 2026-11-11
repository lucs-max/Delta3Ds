#!/usr/bin/env python3
"""
Delta3DS texture converter.

Reads the texture pages of a GameMaker data.win that YOU own, and writes
3DS-ready texture atlases next to it (in a "d3ds" folder). The runner on the
3DS reads the game's code, rooms and sounds straight from data.win; only the
textures need converting, because the 3DS GPU needs tiled textures of at most
1024x1024 pixels.

Usage:
    python3 d3ds_convert.py <game folder> [--out <folder to copy to>]

<game folder> is the folder that contains data.win (for multi-chapter games,
the folder that contains the chapter sub-folders also works: every data.win
found one level down is converted).

With --out, the game files needed on the 3DS (data.win, music, audio groups,
language files, ...) are copied there together with the converted textures,
ready to be put on the SD card at sdmc:/3ds/Delta3DS/.

Requirements: Python 3.8+, Pillow, numpy  (pip install pillow numpy)
"""
import argparse
import bz2
import io
import os
import shutil
import struct
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("This tool needs Pillow and numpy:  pip install pillow numpy")

ATLAS_SIZE = 1024
PAD = 1

# --------------------------------------------------------------------- reading


class DataWin:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        if self.d[:4] != b"FORM":
            raise ValueError("%s is not a GameMaker data file" % path)
        self.chunks = {}
        pos = 8
        end = 8 + self.u32(4)
        while pos + 8 <= min(end, len(self.d)):
            name = self.d[pos:pos + 4].decode("latin-1")
            ln = self.u32(pos + 4)
            self.chunks[name] = (pos + 8, ln)
            pos += 8 + ln

    def u32(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def u16(self, o):
        return struct.unpack_from("<H", self.d, o)[0]

    def i16(self, o):
        return struct.unpack_from("<h", self.d, o)[0]

    def plist(self, off):
        n = self.u32(off)
        return [self.u32(off + 4 + i * 4) for i in range(n)]

    # -- textures
    def looks_like_image(self, p):
        if p <= 0 or p + 8 > len(self.d):
            return False
        m = self.d[p:p + 4]
        return m in (b"\x89PNG", b"fioq", b"2zoq")

    def texture_data_pointers(self):
        if "TXTR" not in self.chunks:
            return []
        off, ln = self.chunks["TXTR"]
        entries = self.plist(off)
        ptrs = []
        for i, e in enumerate(entries):
            found = 0
            # the data pointer is the last field of the entry; entry size
            # depends on the GameMaker version, so try each candidate
            for size in (8, 12, 16, 28, 32, 24, 20):
                if e + size > len(self.d):
                    continue
                cand = self.u32(e + size - 4)
                if self.looks_like_image(cand):
                    found = cand
                    break
            if not found:
                print("  warning: texture page %d has no embedded image (external texture?)" % i)
            ptrs.append(found)
        return ptrs

    def decode_image(self, p, limit):
        magic = self.d[p:p + 4]
        if magic == b"\x89PNG":
            return Image.open(io.BytesIO(self.d[p:limit])).convert("RGBA")
        if magic == b"fioq":
            w, h, length = struct.unpack_from("<HHI", self.d, p + 4)
            return qoi_decode(self.d, p + 12, length, w, h)
        if magic == b"2zoq":
            w, h = struct.unpack_from("<HH", self.d, p + 4)
            start = p + 8
            if self.d[start:start + 3] != b"BZh":
                start += 4  # 2022.5+: uncompressed size field
            dec = bz2.BZ2Decompressor()
            raw = dec.decompress(self.d[start:limit])
            if raw[:4] != b"fioq":
                raise ValueError("bad bz2 qoi texture")
            w2, h2, length = struct.unpack_from("<HHI", raw, 4)
            return qoi_decode(raw, 12, length, w2, h2)
        raise ValueError("unknown texture format at %d" % p)

    def tpag_items(self):
        if "TPAG" not in self.chunks:
            return []
        off, ln = self.chunks["TPAG"]
        items = []
        for p in self.plist(off):
            sx, sy, sw, sh = (self.u16(p + k) for k in (0, 2, 4, 6))
            page = self.i16(p + 20)
            items.append((sx, sy, sw, sh, page))
        return items


def _sx(v, bits):
    """Two's complement sign extension of a small bit field."""
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def qoi_decode(buf, pos, length, w, h):
    """GameMaker's QOI variant (based on the 2021 QOI draft)."""
    out = bytearray(w * h * 4)
    index = [0] * 256
    r = g = b = 0
    a = 255
    run = 0
    end = pos + length
    o = 0
    total = w * h * 4
    mv = buf
    while o < total:
        if run > 0:
            run -= 1
        elif pos < end:
            b1 = mv[pos]
            pos += 1
            if (b1 & 0xC0) == 0x00:
                i = (b1 & 0x3F) << 2
                r, g, b, a = index[i], index[i + 1], index[i + 2], index[i + 3]
            elif (b1 & 0xE0) == 0x40:
                run = b1 & 0x1F
            elif (b1 & 0xE0) == 0x60:
                b2 = mv[pos]
                pos += 1
                run = (((b1 & 0x1F) << 8) | b2) + 32
            elif (b1 & 0xC0) == 0x80:
                r = (r + _sx((b1 >> 4) & 3, 2)) & 255
                g = (g + _sx((b1 >> 2) & 3, 2)) & 255
                b = (b + _sx(b1 & 3, 2)) & 255
            elif (b1 & 0xE0) == 0xC0:
                b2 = mv[pos]
                pos += 1
                r = (r + _sx(b1 & 0x1F, 5)) & 255
                g = (g + _sx(b2 >> 4, 4)) & 255
                b = (b + _sx(b2 & 15, 4)) & 255
            elif (b1 & 0xF0) == 0xE0:
                b2, b3 = mv[pos], mv[pos + 1]
                pos += 2
                m = (b1 << 16) | (b2 << 8) | b3
                r = (r + _sx((m >> 15) & 31, 5)) & 255
                g = (g + _sx((m >> 10) & 31, 5)) & 255
                b = (b + _sx((m >> 5) & 31, 5)) & 255
                a = (a + _sx(m & 31, 5)) & 255
            else:
                if b1 & 8:
                    r = mv[pos]; pos += 1
                if b1 & 4:
                    g = mv[pos]; pos += 1
                if b1 & 2:
                    b = mv[pos]; pos += 1
                if b1 & 1:
                    a = mv[pos]; pos += 1
            i = ((r ^ g ^ b ^ a) & 63) << 2
            index[i], index[i + 1], index[i + 2], index[i + 3] = r, g, b, a
        out[o] = r
        out[o + 1] = g
        out[o + 2] = b
        out[o + 3] = a
        o += 4
    return Image.frombytes("RGBA", (w, h), bytes(out))


# --------------------------------------------------------------------- packing


class Shelf:
    def __init__(self, y, h):
        self.y, self.h, self.x = y, h, 0


class AtlasPacker:
    def __init__(self, size):
        self.size = size
        self.atlases = []  # list of (image, shelves, used_height)

    def _new(self):
        img = Image.new("RGBA", (self.size, self.size), (0, 0, 0, 0))
        self.atlases.append([img, [], 0])
        return len(self.atlases) - 1

    def place(self, w, h):
        pw, ph = w + PAD * 2, h + PAD * 2
        for ai, at in enumerate(self.atlases):
            img, shelves, used = at
            for s in shelves:
                if ph <= s.h and s.x + pw <= self.size:
                    x = s.x
                    s.x += pw
                    return ai, x + PAD, s.y + PAD
            if used + ph <= self.size:
                s = Shelf(used, ph)
                s.x = pw
                shelves.append(s)
                at[2] = used + ph
                return ai, PAD, s.y + PAD
        ai = self._new()
        s = Shelf(0, ph)
        s.x = pw
        self.atlases[ai][1].append(s)
        self.atlases[ai][2] = ph
        return ai, PAD, PAD


_swizzle_cache = {}


def swizzle_indices(w, h):
    key = (w, h)
    if key in _swizzle_cache:
        return _swizzle_cache[key]
    ys, xs = np.mgrid[0:h, 0:w]
    tile = (ys >> 3) * (w >> 3) + (xs >> 3)
    within = ((xs & 1) | ((ys & 1) << 1) | ((xs & 2) << 1) | ((ys & 2) << 2) | ((xs & 4) << 2) | ((ys & 4) << 3))
    dst = (tile * 64 + within).reshape(-1)
    _swizzle_cache[key] = dst
    return dst


def write_atlas(img, used_h, path):
    w = img.width
    h = 8
    while h < used_h:
        h *= 2
    h = max(h, 8)
    if h < img.height:
        img = img.crop((0, 0, w, h))
    arr = np.asarray(img, dtype=np.uint8)[::-1]  # bottom-up, like the 3DS GPU expects
    flat = arr.reshape(-1, 4)
    out = np.empty_like(flat)
    out[swizzle_indices(w, h)] = flat
    abgr = out[:, [3, 2, 1, 0]]
    with open(path, "wb") as f:
        f.write(b"D3TX" + struct.pack("<HHI", w, h, 0))
        f.write(abgr.tobytes())
    return h


# --------------------------------------------------------------------- main


def convert(datawin_path, out_dir):
    print("Converting %s" % datawin_path)
    dw = DataWin(datawin_path)
    ptrs = dw.texture_data_pointers()
    items = dw.tpag_items()
    print("  %d texture pages, %d texture items" % (len(ptrs), len(items)))
    # limits for slicing PNG data: next pointer or chunk end
    txtr_end = sum(dw.chunks.get("TXTR", (0, 0)))
    sorted_ptrs = sorted(p for p in ptrs if p)
    pages = {}

    def page_image(i):
        if i in pages:
            return pages[i]
        p = ptrs[i] if 0 <= i < len(ptrs) else 0
        img = None
        if p:
            later = [q for q in sorted_ptrs if q > p]
            limit = later[0] if later else max(txtr_end, len(dw.d))
            try:
                img = dw.decode_image(p, limit)
            except Exception as e:  # keep going; the item becomes a placeholder
                print("  warning: page %d: %s" % (i, e))
        pages.clear()  # keep memory low: one decoded page at a time
        pages[i] = img
        return img

    packer = AtlasPacker(ATLAS_SIZE)
    entries = [None] * len(items)
    order = sorted(range(len(items)), key=lambda k: (items[k][4], -items[k][3], -items[k][2]))
    for n, k in enumerate(order):
        sx, sy, sw, sh, page = items[k]
        if sw == 0 or sh == 0:
            entries[k] = (-1, 0, 0, 0, 0, sw, sh)
            continue
        img = page_image(page)
        if img is None:
            entries[k] = (-1, 0, 0, 0, 0, sw, sh)
            continue
        crop = img.crop((sx, sy, sx + sw, sy + sh))
        w, h = sw, sh
        while w + 2 * PAD > ATLAS_SIZE or h + 2 * PAD > ATLAS_SIZE:
            w, h = max(1, w // 2), max(1, h // 2)
        if (w, h) != (sw, sh):
            crop = crop.resize((w, h), Image.NEAREST)
        ai, x, y = packer.place(w, h)
        packer.atlases[ai][0].paste(crop, (x, y))
        entries[k] = (ai, x, y, w, h, sw, sh)
        if n % 500 == 0:
            print("  packed %d/%d" % (n, len(items)))

    os.makedirs(out_dir, exist_ok=True)
    for fn in os.listdir(out_dir):
        if fn.startswith("atlas_") and fn.endswith(".bin"):
            os.remove(os.path.join(out_dir, fn))
    total = 0
    for ai, (img, shelves, used) in enumerate(packer.atlases):
        h = write_atlas(img, used, os.path.join(out_dir, "atlas_%03d.bin" % ai))
        total += ATLAS_SIZE * h * 4
    with open(os.path.join(out_dir, "atlas.idx"), "wb") as f:
        f.write(b"D3AI" + struct.pack("<III", 1, len(items), len(packer.atlases)))
        for e in entries:
            ai, x, y, w, h, ow, oh = e
            f.write(struct.pack("<hHHHHHH", ai, x, y, w, h, ow, oh))
    print("  wrote %d atlases (%.1f MB) to %s" % (len(packer.atlases), total / 1048576.0, out_dir))


SKIP_EXT = {".exe", ".dll", ".pdb", ".so", ".dylib"}


def copy_game_files(src, dst):
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d != "d3ds" and not d.startswith(".")]
        rel = os.path.relpath(root, src)
        target = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(target, exist_ok=True)
        for fn in files:
            if os.path.splitext(fn)[1].lower() in SKIP_EXT:
                continue
            s, t = os.path.join(root, fn), os.path.join(target, fn)
            if not os.path.exists(t) or os.path.getsize(t) != os.path.getsize(s):
                shutil.copy2(s, t)


def find_datawins(folder):
    found = []
    if os.path.isfile(os.path.join(folder, "data.win")):
        found.append(folder)
    for d in sorted(os.listdir(folder)):
        sub = os.path.join(folder, d)
        if os.path.isdir(sub) and os.path.isfile(os.path.join(sub, "data.win")):
            found.append(sub)
    return found


def main():
    ap = argparse.ArgumentParser(description="Convert GameMaker textures for Delta3DS")
    ap.add_argument("game_folder", help="folder containing data.win (or chapter sub-folders)")
    ap.add_argument("--out", help="copy the game and converted textures to this folder (SD card layout)")
    args = ap.parse_args()
    src = os.path.abspath(args.game_folder)
    if os.path.isfile(src):
        src = os.path.dirname(src)
    folders = find_datawins(src)
    if not folders:
        sys.exit("No data.win found in %s" % src)
    dst_root = os.path.abspath(args.out) if args.out else src
    if args.out:
        print("Copying game files to %s" % dst_root)
        copy_game_files(src, dst_root)
    for folder in folders:
        rel = os.path.relpath(folder, src)
        target = os.path.join(dst_root, rel) if rel != "." else dst_root
        convert(os.path.join(folder, "data.win"), os.path.join(target, "d3ds"))
    print("Done. Put the folder on your SD card as sdmc:/3ds/Delta3DS/")


if __name__ == "__main__":
    main()
