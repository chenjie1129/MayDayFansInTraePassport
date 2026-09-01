#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate the 8 "tribe icon" sprites for FoloToy AI Passport (ESP32-C3, 240x320, LVGL 9).

Icon pack: mayday (index 0..7)
  0 carrot     萝卜   - full-band / default
  1 rabbit     兔子   - band nickname
  2 ball_pink  粉球   - plush, #FF7FB0
  3 ball_red   红球   - plush, #E02020
  4 ball_green 绿球   - plush
  5 ball_blue  蓝球   - plush
  6 ball_yellow黄球   - plush
  7 nineball   9号球  - HARD billiard, must stay distinct from #6

Design rule that drives the whole file:
  index 2..6 are rendered as PLUSH  -> fuzzy edge, soft highlight, fur noise, anti-alias off
  index 7    is rendered as HARD    -> crisp anti-aliased rim, white number circle,
                                      black digit "9", strong specular dot
  => #6 and #7 are separated by TEXTURE + DIGIT, never by hue alone.

Outputs (all under ../):
  png/NN_name.png            32x32 RGBA sprites
  preview/*.png              human-checkable preview sheets (1x, x10, dark bg)
  tribe_icons_mayday.c/.h    LVGL 9 lv_image_dsc_t, LV_COLOR_FORMAT_RGB565A8

The carrot at index 0 is adapted from the personal, non-commercial Bubu
reference documented in ../source/NOTICE.md. The other seven icons are
generated from generic geometry.
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

W = H = 32
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG_DIR = os.path.join(ROOT, "png")
PREV_DIR = os.path.join(ROOT, "preview")
REFERENCE_PATH = os.path.join(ROOT, "source", "bubu_reference_idle.png")
RESAMPLE_LANCZOS = getattr(Image, "Resampling", Image).LANCZOS

PAPER = (237, 230, 214)      # light UI background used for preview
DARK = (24, 24, 28)          # dark background used for preview

# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------


def clamp(v, lo=0, hi=255):
    return lo if v < lo else (hi if v > hi else v)


def shade(rgb, f):
    """Multiply brightness by f."""
    return tuple(clamp(int(round(c * f))) for c in rgb)


def mix(c1, c2, t):
    """Linear blend c1 -> c2 by t in [0,1]."""
    return tuple(clamp(int(round(a + (b - a) * t))) for a, b in zip(c1, c2))


def noise(x, y, seed=0):
    """Deterministic value noise in [0,1) -- keeps the build reproducible."""
    h = (x * 73856093) ^ (y * 19349663) ^ (seed * 83492791)
    h &= 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFF) / 65536.0


class Canvas:
    def __init__(self):
        self.px = [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]

    def put(self, x, y, rgb, a=255):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = (rgb[0], rgb[1], rgb[2], a)

    def get(self, x, y):
        if 0 <= x < W and 0 <= y < H:
            return self.px[y][x]
        return (0, 0, 0, 0)

    def alpha(self, x, y):
        return self.get(x, y)[3]

    def to_image(self):
        im = Image.new("RGBA", (W, H))
        im.putdata([self.px[y][x] for y in range(H) for x in range(W)])
        return im

    def outline(self, rgb, thresh=128):
        """Grow the opaque silhouette by 1px outward in `rgb`."""
        add = []
        for y in range(H):
            for x in range(W):
                if self.alpha(x, y) >= thresh:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if self.alpha(x + dx, y + dy) >= thresh:
                        add.append((x, y))
                        break
        for x, y in add:
            self.put(x, y, rgb, 255)


def cov_circle(x, y, cx, cy, r, n=4):
    """Sub-pixel coverage of pixel (x,y) by a circle."""
    hit = 0
    for sy in range(n):
        for sx in range(n):
            px = x + (sx + 0.5) / n
            py = y + (sy + 0.5) / n
            if (px - cx) ** 2 + (py - cy) ** 2 <= r * r:
                hit += 1
    return hit / (n * n)


def cov_ellipse(x, y, cx, cy, rx, ry, ang=0.0, n=4):
    """Sub-pixel coverage by a (rotated) ellipse. ang in degrees."""
    ca = math.cos(math.radians(ang))
    sa = math.sin(math.radians(ang))
    hit = 0
    for sy in range(n):
        for sx in range(n):
            px = x + (sx + 0.5) / n - cx
            py = y + (sy + 0.5) / n - cy
            u = px * ca + py * sa
            v = -px * sa + py * ca
            if (u / rx) ** 2 + (v / ry) ** 2 <= 1.0:
                hit += 1
    return hit / (n * n)


# --------------------------------------------------------------------------
# 2..6  plush balls
# --------------------------------------------------------------------------

CX = CY = 15.5
BALL_R = 13.1


def draw_plush_ball(base, seed):
    """Fuzzy fabric ball: no anti-aliasing, noisy rim, soft highlight, fur grain."""
    c = Canvas()
    rim = shade(base, 0.62)
    lit = mix(base, (255, 255, 255), 0.55)

    for y in range(H):
        for x in range(W):
            cov = cov_circle(x, y, CX, CY, BALL_R)
            d = math.hypot(x + 0.5 - CX, y + 0.5 - CY)
            n_edge = noise(x, y, seed)

            solid = cov >= 0.55
            if not solid:
                # fuzzy fabric rim: partially covered pixels survive by noise
                if cov > 0.02 and n_edge > 0.42:
                    solid = True
                # a few stray fur strands just outside the body
                elif BALL_R < d <= BALL_R + 1.35 and noise(x, y, seed + 41) > 0.86:
                    solid = True
            if not solid:
                continue

            # volume: light from upper-left
            l = ((CX - (x + 0.5)) + (CY - (y + 0.5))) / (2.0 * BALL_R)
            f = 1.0 + 0.30 * l
            # fur grain
            f *= 0.94 + 0.13 * noise(x, y, seed + 7)
            col = shade(base, f)

            # darker rim for definition on light paper background
            if d > BALL_R - 2.3:
                col = mix(col, rim, min(1.0, (d - (BALL_R - 2.3)) / 2.6) * 0.85)

            # soft plush highlight (blurry, not specular)
            dh = math.hypot(x + 0.5 - 11.0, y + 0.5 - 10.6)
            if dh < 4.2:
                col = mix(col, lit, (1.0 - dh / 4.2) * 0.50)

            c.put(x, y, col, 255)
    return c


# --------------------------------------------------------------------------
# 7  nine-ball (hard billiard)
# --------------------------------------------------------------------------

NINE_GLYPH = [
    ".####.",
    "#....#",
    "#....#",
    "#....#",
    ".#####",
    ".....#",
    ".....#",
    "#....#",
    ".####.",
]

BAND_HALF = 5.6          # yellow stripe half-height
NUM_CIRCLE_R = 6.3       # white number circle


def draw_nineball():
    c = Canvas()
    white = (250, 250, 248)
    band = (255, 196, 0)
    ink = (26, 26, 26)
    rim = (150, 146, 138)

    for y in range(H):
        for x in range(W):
            cov = cov_circle(x, y, CX, CY, BALL_R)
            if cov <= 0.02:
                continue

            d = math.hypot(x + 0.5 - CX, y + 0.5 - CY)
            in_band = abs(y + 0.5 - CY) <= BAND_HALF
            in_num = d <= NUM_CIRCLE_R

            base = band if (in_band and not in_num) else white

            # hard glossy shading
            l = ((CX - (x + 0.5)) + (CY - (y + 0.5))) / (2.0 * BALL_R)
            col = shade(base, 1.0 + 0.16 * l)

            # crisp dark rim -> reads as a hard sphere, not fabric
            if d > BALL_R - 2.0:
                col = mix(col, rim, min(1.0, (d - (BALL_R - 2.0)) / 2.2))

            # strong specular dot (the "hard" cue plush balls never get)
            ds = math.hypot(x + 0.5 - 10.6, y + 0.5 - 9.8)
            if ds < 2.9:
                col = mix(col, (255, 255, 255), (1.0 - ds / 2.9) * 0.95)

            # anti-aliased outer edge
            c.put(x, y, col, 255 if cov >= 0.5 else int(round(255 * cov)))

    # black digit "9", centred inside the white circle
    gw = len(NINE_GLYPH[0])
    gh = len(NINE_GLYPH)
    gx0 = int(round(CX + 0.5 - gw / 2.0))
    gy0 = int(round(CY + 0.5 - gh / 2.0))
    for r, row in enumerate(NINE_GLYPH):
        for k, ch in enumerate(row):
            if ch == "#":
                c.put(gx0 + k, gy0 + r, ink, 255)
    return c


# --------------------------------------------------------------------------
# 0  carrot
# --------------------------------------------------------------------------


def draw_carrot():
    if not os.path.exists(REFERENCE_PATH):
        raise SystemExit("missing reference artwork: %s" % REFERENCE_PATH)
    source = Image.open(REFERENCE_PATH).convert("RGBA")
    sprite = source.resize((29, 31), RESAMPLE_LANCZOS)
    stage = Image.new("RGBA", (W, H))
    stage.alpha_composite(sprite, (1, 0))

    c = Canvas()
    data = list(stage.getdata())
    c.px = [data[y * W:(y + 1) * W] for y in range(H)]
    return c


# --------------------------------------------------------------------------
# 1  rabbit head
# --------------------------------------------------------------------------


def draw_rabbit():
    c = Canvas()
    fur = (248, 246, 242)
    fur_d = (214, 210, 202)
    pink = (255, 158, 196)
    ink = (43, 43, 48)

    shapes = (
        (16.0, 21.0, 8.2, 6.9, 0.0),      # head
        (11.7, 9.4, 2.6, 7.1, 13.0),      # left ear
        (20.3, 9.4, 2.6, 7.1, -13.0),     # right ear
    )
    for y in range(H):
        for x in range(W):
            cov = max(cov_ellipse(x, y, *s) for s in shapes)
            if cov < 0.5:
                continue
            l = ((16.0 - (x + 0.5)) + (21.0 - (y + 0.5))) / 20.0
            c.put(x, y, shade(fur, 1.0 + 0.05 * l), 255)

    # shade the underside of the head so it is not a flat blob
    for y in range(H):
        for x in range(W):
            if c.alpha(x, y) == 0:
                continue
            if cov_ellipse(x, y, 16.0, 21.0, 8.2, 6.9) >= 0.5 and y >= 24:
                c.put(x, y, mix(fur, fur_d, (y - 23) / 4.0), 255)

    # inner ears
    for cx, ang in ((11.7, 13.0), (20.3, -13.0)):
        for y in range(H):
            for x in range(W):
                if cov_ellipse(x, y, cx, 9.6, 1.2, 4.6, ang) >= 0.5:
                    c.put(x, y, pink, 255)

    # eyes + nose
    for y in range(H):
        for x in range(W):
            if (cov_ellipse(x, y, 12.9, 20.4, 1.15, 1.5) >= 0.5
                    or cov_ellipse(x, y, 19.1, 20.4, 1.15, 1.5) >= 0.5):
                c.put(x, y, ink, 255)
            elif cov_ellipse(x, y, 16.0, 23.2, 1.5, 1.05) >= 0.5:
                c.put(x, y, pink, 255)

    # Outline, but never inside the notch between the two ears: filling that
    # gap produced a stray grey seam down the middle of the head.
    before = [[c.alpha(x, y) for x in range(W)] for y in range(H)]
    c.outline((150, 146, 138))
    for y in range(H):
        for x in range(W):
            if before[y][x] != 0 or c.alpha(x, y) == 0:
                continue
            near_head = cov_ellipse(x, y, 16.0, 21.0, 9.6, 8.3) >= 0.5
            between_ears = 13 <= x <= 18 and y <= 15
            if between_ears and not near_head:
                c.put(x, y, (0, 0, 0), 0)
    return c


# --------------------------------------------------------------------------
# RGB565 conversion + LVGL C export
# --------------------------------------------------------------------------


def to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def from565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def emit_c(icons):
    """icons: list of (index, name, PIL RGBA image)."""
    lines = [
        "// Auto-generated by tools/gen_icons.py -- do not edit by hand.",
        "// Tribe icon pack: mayday. 8 sprites, 32x32, LVGL LV_COLOR_FORMAT_RGB565A8.",
        "// Layout per image: w*h RGB565 little-endian pixels, then w*h A8 bytes.",
        "",
        '#include "tribe_icons_mayday.h"',
        "",
    ]
    for idx, name, im in icons:
        px = list(im.getdata())
        color_bytes = []
        alpha_bytes = []
        for (r, g, b, a) in px:
            v = to565(r, g, b)
            color_bytes.append(v & 0xFF)
            color_bytes.append((v >> 8) & 0xFF)
            alpha_bytes.append(a)
        data = color_bytes + alpha_bytes
        sym = "tribe_icon_%s" % name

        lines.append("static const uint8_t %s_map[] = {" % sym)
        for i in range(0, len(data), 16):
            chunk = ", ".join("0x%02X" % b for b in data[i:i + 16])
            lines.append("    %s," % chunk)
        lines.append("};")
        lines.append("")
        lines.append("const lv_image_dsc_t %s = {" % sym)
        lines.append("    .header = {")
        lines.append("        .magic  = LV_IMAGE_HEADER_MAGIC,")
        lines.append("        .cf     = LV_COLOR_FORMAT_RGB565A8,")
        lines.append("        .flags  = 0,")
        lines.append("        .w      = %d," % W)
        lines.append("        .h      = %d," % H)
        lines.append("        .stride = %d," % (W * 2))
        lines.append("    },")
        lines.append("    .data_size = sizeof(%s_map)," % sym)
        lines.append("    .data      = %s_map," % sym)
        lines.append("};")
        lines.append("")

    lines.append("const lv_image_dsc_t *const tribe_icon_pack_mayday[TRIBE_ICON_COUNT] = {")
    for idx, name, _ in icons:
        lines.append("    [%d] = &tribe_icon_%s," % (idx, name))
    lines.append("};")
    lines.append("")

    with open(os.path.join(ROOT, "tribe_icons_mayday.c"), "w") as f:
        f.write("\n".join(lines))

    hdr = [
        "// Auto-generated by tools/gen_icons.py -- do not edit by hand.",
        "#ifndef TRIBE_ICONS_MAYDAY_H",
        "#define TRIBE_ICONS_MAYDAY_H",
        "",
        '#include "lvgl.h"',
        "",
        "#define TRIBE_ICON_W %d" % W,
        "#define TRIBE_ICON_H %d" % H,
        "#define TRIBE_ICON_COUNT %d" % len(icons),
        "",
        "// Icon indices travel inside the BLE advert payload (3 bits, 0..7).",
        "// Index 7 is a MOOD signal, not an identity -- callers must route it",
        "// to the quiet feedback branch instead of the encounter animation.",
        "enum {",
    ]
    enum_names = {
        "carrot": "TRIBE_ICON_CARROT",
        "rabbit": "TRIBE_ICON_RABBIT",
        "ball_pink": "TRIBE_ICON_BALL_PINK",
        "ball_red": "TRIBE_ICON_BALL_RED",
        "ball_green": "TRIBE_ICON_BALL_GREEN",
        "ball_blue": "TRIBE_ICON_BALL_BLUE",
        "ball_yellow": "TRIBE_ICON_BALL_YELLOW",
        "nineball": "TRIBE_ICON_NINEBALL",
    }
    for idx, name, _ in icons:
        hdr.append("    %s = %d," % (enum_names[name], idx))
    hdr += [
        "};",
        "",
    ]
    for idx, name, _ in icons:
        hdr.append("extern const lv_image_dsc_t tribe_icon_%s;" % name)
    hdr += [
        "",
        "extern const lv_image_dsc_t *const tribe_icon_pack_mayday[TRIBE_ICON_COUNT];",
        "",
        "#endif // TRIBE_ICONS_MAYDAY_H",
        "",
    ]
    with open(os.path.join(ROOT, "tribe_icons_mayday.h"), "w") as f:
        f.write("\n".join(hdr))


# --------------------------------------------------------------------------
# preview sheets
# --------------------------------------------------------------------------


def pick_font(size):
    cands = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    ]
    for p in cands:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def make_previews(icons):
    labels = {
        "carrot": "0 CARROT",
        "rabbit": "1 RABBIT",
        "ball_pink": "2 PINK",
        "ball_red": "3 RED",
        "ball_green": "4 GREEN",
        "ball_blue": "5 BLUE",
        "ball_yellow": "6 YELLOW",
        "nineball": "7 NINE-BALL",
    }

    # --- 1x strip: the honest, real-size view ---
    gap = 3
    sw = len(icons) * W + (len(icons) + 1) * gap
    strip = Image.new("RGB", (sw, H + 2 * gap), PAPER)
    for i, (_, _, im) in enumerate(icons):
        strip.paste(im, (gap + i * (W + gap), gap), im)
    strip.save(os.path.join(PREV_DIR, "strip_1x.png"))
    strip.resize((sw * 6, (H + 2 * gap) * 6), Image.NEAREST).save(
        os.path.join(PREV_DIR, "strip_x6.png"))

    # --- big grid with labels ---
    scale, cols = 9, 4
    cell_w, cell_h = W * scale + 26, H * scale + 52
    rows = (len(icons) + cols - 1) // cols
    for bg, fg, fname in ((PAPER, (40, 38, 34), "grid_light_x9.png"),
                          (DARK, (232, 230, 226), "grid_dark_x9.png")):
        sheet = Image.new("RGB", (cell_w * cols, cell_h * rows), bg)
        dr = ImageDraw.Draw(sheet)
        font = pick_font(20)
        for i, (_, name, im) in enumerate(icons):
            r, cc = divmod(i, cols)
            ox = cc * cell_w + 13
            oy = r * cell_h + 13
            big = im.resize((W * scale, H * scale), Image.NEAREST)
            sheet.paste(big, (ox, oy), big)
            dr.rectangle([ox - 1, oy - 1, ox + W * scale, oy + H * scale],
                         outline=(150, 146, 138))
            dr.text((ox, oy + H * scale + 9), labels[name], fill=fg, font=font)
        sheet.save(os.path.join(PREV_DIR, fname))

    # --- the pair that must not collide: #6 vs #7 ---
    pair = [ic for ic in icons if ic[1] in ("ball_yellow", "nineball")]
    ps = Image.new("RGB", (2 * (W * 12 + 20) + 20, H * 12 + 70), PAPER)
    dr = ImageDraw.Draw(ps)
    font = pick_font(22)
    for i, (_, name, im) in enumerate(pair):
        ox = 20 + i * (W * 12 + 20)
        big = im.resize((W * 12, H * 12), Image.NEAREST)
        ps.paste(big, (ox, 20), big)
        dr.text((ox, 20 + H * 12 + 12), labels[name], fill=(40, 38, 34), font=font)
    ps.save(os.path.join(PREV_DIR, "collision_check_6_vs_7.png"))


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main():
    os.makedirs(PNG_DIR, exist_ok=True)
    os.makedirs(PREV_DIR, exist_ok=True)

    spec = [
        (0, "carrot", draw_carrot()),
        (1, "rabbit", draw_rabbit()),
        (2, "ball_pink", draw_plush_ball((255, 127, 176), 11)),
        (3, "ball_red", draw_plush_ball((224, 32, 32), 23)),
        (4, "ball_green", draw_plush_ball((47, 191, 79), 37)),
        (5, "ball_blue", draw_plush_ball((47, 143, 239), 53)),
        (6, "ball_yellow", draw_plush_ball((255, 210, 30), 71)),
        (7, "nineball", draw_nineball()),
    ]

    icons = []
    for idx, name, cv in spec:
        im = cv.to_image()
        im.save(os.path.join(PNG_DIR, "%02d_%s.png" % (idx, name)))
        icons.append((idx, name, im))

    emit_c(icons)
    make_previews(icons)

    # ---- report: opaque pixel count, unique colours, RGB565 round-trip ----
    print("idx name          opaque  uniq565  base RGB565 round-trip")
    for idx, name, im in icons:
        px = list(im.getdata())
        op = [p for p in px if p[3] > 0]
        uniq = {to565(r, g, b) for (r, g, b, a) in op}
        print("%3d %-13s %6d %8d" % (idx, name, len(op), len(uniq)))
    per = W * H * 3
    print("\nRGB565A8 flash: %d B/icon x 8 = %d B (%.1f KB)"
          % (per, per * 8, per * 8 / 1024.0))
    print("I4 alternative: %d B/icon + 64 B palette x 8 = %d B (%.1f KB)"
          % (W * H // 2, (W * H // 2 + 64) * 8, (W * H // 2 + 64) * 8 / 1024.0))
    for lbl, base in (("pink #FF7FB0", (255, 127, 176)), ("red  #E02020", (224, 32, 32)),
                      ("yellow plush", (255, 210, 30)), ("nine band", (255, 196, 0))):
        v = to565(*base)
        print("  %-13s -> 0x%04X -> %s" % (lbl, v, from565(v)))


if __name__ == "__main__":
    main()
