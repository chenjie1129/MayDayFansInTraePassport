#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Verify tribe_icons_mayday.c by a path INDEPENDENT of the generator.

The generator wrote the .c file from PIL images. This script does the reverse:
it parses the emitted C source as text, rebuilds each sprite from the RGB565 +
A8 bytes, and checks the result against the PNGs. If the byte layout, stride,
endianness or alpha plane were wrong, the round-trip below would break.

Checks per icon:
  1. data_size == w*h*3 (RGB565 2B + A8 1B)
  2. decoded alpha plane matches the PNG alpha exactly
  3. decoded colour equals the PNG colour after the same 565 quantisation
  4. every declared header field matches the real 32x32 / stride 64
Plus a global check that #6 and #7 are actually distinguishable.
"""

import os
import re
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C_FILE = os.path.join(ROOT, "tribe_icons_mayday.c")
H_FILE = os.path.join(ROOT, "tribe_icons_mayday.h")
PNG_DIR = os.path.join(ROOT, "png")

W = H = 32
NAMES = [
    "carrot", "rabbit", "ball_pink", "ball_red",
    "ball_green", "ball_blue", "ball_yellow", "nineball",
]


def q565(r, g, b):
    """Quantise to RGB565 then expand back, exactly as hardware would show it."""
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    rr = (v >> 11) & 0x1F
    gg = (v >> 5) & 0x3F
    bb = v & 0x1F
    return (rr * 255 // 31, gg * 255 // 63, bb * 255 // 31)


def parse_c(src, name):
    """Pull the byte array + header fields for one icon out of the C text."""
    sym = "tribe_icon_%s" % name
    m = re.search(r"static const uint8_t %s_map\[\]\s*=\s*\{(.*?)\};" % sym, src, re.S)
    if not m:
        raise AssertionError("array %s_map not found" % sym)
    data = [int(t, 16) for t in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]

    m2 = re.search(r"const lv_image_dsc_t %s\s*=\s*\{(.*?)\};" % sym, src, re.S)
    if not m2:
        raise AssertionError("descriptor %s not found" % sym)
    blk = m2.group(1)
    fields = {}
    for key in ("w", "h", "stride", "cf", "magic"):
        mm = re.search(r"\.%s\s*=\s*([A-Za-z0-9_]+)" % key, blk)
        if mm:
            fields[key] = mm.group(1)
    return data, fields


def main():
    for p in (C_FILE, H_FILE):
        if not os.path.exists(p):
            print("MISSING %s" % p)
            return 1
    src = open(C_FILE).read()
    hdr = open(H_FILE).read()

    failures = []
    decoded = {}
    expect = W * H * 3

    print("icon           bytes   hdr(w,h,stride)      alpha  colour")
    for idx, name in enumerate(NAMES):
        data, f = parse_c(src, name)
        png = Image.open(os.path.join(PNG_DIR, "%02d_%s.png" % (idx, name))).convert("RGBA")
        ref = list(png.getdata())

        ok_size = (len(data) == expect)
        ok_hdr = (f.get("w") == str(W) and f.get("h") == str(H)
                  and f.get("stride") == str(W * 2)
                  and f.get("cf") == "LV_COLOR_FORMAT_RGB565A8")
        if not ok_size:
            failures.append("%s: data_size %d != %d" % (name, len(data), expect))
        if not ok_hdr:
            failures.append("%s: bad header %s" % (name, f))

        # rebuild from the C bytes
        col_plane = data[:W * H * 2]
        a_plane = data[W * H * 2:]
        bad_a = bad_c = 0
        rebuilt = Image.new("RGBA", (W, H))
        out = []
        for i in range(W * H):
            lo = col_plane[2 * i]
            hi = col_plane[2 * i + 1]
            v = lo | (hi << 8)
            rr = ((v >> 11) & 0x1F) * 255 // 31
            gg = ((v >> 5) & 0x3F) * 255 // 63
            bb = (v & 0x1F) * 255 // 31
            aa = a_plane[i]
            out.append((rr, gg, bb, aa))

            r0, g0, b0, a0 = ref[i]
            if aa != a0:
                bad_a += 1
            if a0 > 0 and (rr, gg, bb) != q565(r0, g0, b0):
                bad_c += 1
        rebuilt.putdata(out)
        decoded[name] = rebuilt

        if bad_a:
            failures.append("%s: %d alpha mismatches" % (name, bad_a))
        if bad_c:
            failures.append("%s: %d colour mismatches" % (name, bad_c))

        print("%-14s %5d   (%s,%s,%s)%s  %5s  %6s"
              % (name, len(data), f.get("w"), f.get("h"), f.get("stride"),
                 "" if ok_hdr else " BAD",
                 "OK" if bad_a == 0 else "x%d" % bad_a,
                 "OK" if bad_c == 0 else "x%d" % bad_c))

    # header enum sanity
    for want in ("TRIBE_ICON_CARROT = 0", "TRIBE_ICON_NINEBALL = 7",
                 "TRIBE_ICON_COUNT 8"):
        if want not in hdr:
            failures.append("header missing: %s" % want)

    # the collision that actually matters: #6 yellow plush vs #7 nine-ball
    y = decoded["ball_yellow"]
    n = decoded["nineball"]
    yp, np_ = list(y.getdata()), list(n.getdata())
    diff = sum(1 for a, b in zip(yp, np_) if a != b)
    # count near-white and near-black pixels: the nine-ball must have both
    n_white = sum(1 for (r, g, b, a) in np_ if a > 0 and r > 200 and g > 200 and b > 200)
    n_dark = sum(1 for (r, g, b, a) in np_ if a > 0 and r < 90 and g < 90 and b < 90)
    y_white = sum(1 for (r, g, b, a) in yp if a > 0 and r > 200 and g > 200 and b > 200)
    y_dark = sum(1 for (r, g, b, a) in yp if a > 0 and r < 90 and g < 90 and b < 90)
    print("\n#6 vs #7 differing pixels: %d / %d" % (diff, W * H))
    print("  nine-ball  white=%3d  dark=%3d   (needs both > 0)" % (n_white, n_dark))
    print("  yellow     white=%3d  dark=%3d   (dark should be ~0)" % (y_white, y_dark))
    if diff < 400:
        failures.append("#6/#7 too similar (%d differing px)" % diff)
    if n_white < 60 or n_dark < 20:
        failures.append("nine-ball lacks white circle / black digit")
    if y_dark > 15:
        failures.append("yellow plush unexpectedly has dark pixels")

    # save a side-by-side rebuilt-from-C proof sheet
    proof = Image.new("RGB", (8 * (W + 4) + 4, W + 8), (237, 230, 214))
    for i, nm in enumerate(NAMES):
        proof.paste(decoded[nm], (4 + i * (W + 4), 4), decoded[nm])
    proof = proof.resize((proof.width * 5, proof.height * 5), Image.NEAREST)
    proof.save(os.path.join(ROOT, "preview", "rebuilt_from_c.png"))
    print("\nwrote preview/rebuilt_from_c.png (decoded straight from the C array)")

    print()
    if failures:
        print("FAIL (%d)" % len(failures))
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- all 8 icons round-trip byte-exactly from tribe_icons_mayday.c")
    return 0


if __name__ == "__main__":
    sys.exit(main())
