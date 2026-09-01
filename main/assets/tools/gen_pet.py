#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate the pet sprites for AI Passport "place-memory" play.

Two asset families:

  FORMS (6) -- 48x48, evolution driven ONLY by the number of discovered places
    0 seed     种子卜  initial          seed buried in soil, tip showing
    1 sprout   芽卜    2 places         breaks soil, two small leaves
    2 young    幼卜    4 places         short carrot body
    3 bubu     卜卜    8 places         standard form
    4 traveler 旅卜    12 places        scarf + small backpack
    5 world    世界卜  16 places        halo of small stars

  ACCESSORIES (5) -- 16x16, personality layer, drawn ON TOP of a form
    0 homebody 宅卜   single place > 70% of dwell   pillow
    1 rover    游卜   >=6 places, avg dwell < 30m   dust puffs
    2 social   社卜   >=30 encounters               floating hearts
    3 solo     独卜   >=8 places, <5 encounters     quiet dot
    4 arena    场卜   >=1 gathering event           mini glow stick

Key constraint that shapes this file:
  Accessories must composite onto forms WITHOUT covering the face. Each form
  therefore publishes an anchor (x, y) in its own 48x48 space, and every
  accessory is blitted at that anchor. Anchors are exported to C so the
  firmware never hardcodes offsets.

Output (under ../):
  png_pet/, preview_pet/, pet_sprites.c/.h

The six forms share the round Bubu visual reference documented in
../source/NOTICE.md. Personality accessories remain locally generated.
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

FW = FH = 48          # form canvas
AW = AH = 16          # accessory canvas

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG_DIR = os.path.join(ROOT, "png_pet")
PREV_DIR = os.path.join(ROOT, "preview_pet")
REFERENCE_PATH = os.path.join(ROOT, "source", "bubu_reference_idle.png")
RESAMPLE_LANCZOS = getattr(Image, "Resampling", Image).LANCZOS

PAPER = (237, 230, 214)
DARK = (24, 24, 28)

# palette
ORANGE = (242, 107, 31)
ORANGE_D = (198, 74, 18)
RIDGE = (176, 62, 14)
LEAF = (63, 163, 63)
LEAF_D = (38, 112, 44)
SOIL = (122, 84, 56)
SOIL_D = (92, 62, 40)
SEEDC = (214, 176, 116)
INK = (43, 43, 48)
WHITE = (250, 250, 248)
PINKC = (255, 158, 196)
SCARF = (219, 58, 82)
PACK = (120, 88, 62)
STAR = (255, 214, 74)
OUTL = (120, 52, 14)


def clamp(v, lo=0, hi=255):
    return lo if v < lo else (hi if v > hi else v)


def shade(rgb, f):
    return tuple(clamp(int(round(c * f))) for c in rgb)


def mix(c1, c2, t):
    return tuple(clamp(int(round(a + (b - a) * t))) for a, b in zip(c1, c2))


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[(0, 0, 0, 0) for _ in range(w)] for _ in range(h)]

    def put(self, x, y, rgb, a=255):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = (rgb[0], rgb[1], rgb[2], a)

    def alpha(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y][x][3]
        return 0

    def to_image(self):
        im = Image.new("RGBA", (self.w, self.h))
        im.putdata([self.px[y][x] for y in range(self.h) for x in range(self.w)])
        return im

    @classmethod
    def from_image(cls, image):
        image = image.convert("RGBA")
        c = cls(*image.size)
        data = list(image.getdata())
        c.px = [data[y * c.w:(y + 1) * c.w] for y in range(c.h)]
        return c

    def outline(self, rgb, skip=None):
        add = []
        for y in range(self.h):
            for x in range(self.w):
                if self.alpha(x, y) >= 128:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if self.alpha(x + dx, y + dy) >= 128:
                        add.append((x, y))
                        break
        for x, y in add:
            if skip and skip(x, y):
                continue
            self.put(x, y, rgb, 255)


def cov_ellipse(x, y, cx, cy, rx, ry, ang=0.0, n=3):
    ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
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


def fill_ellipse(c, cx, cy, rx, ry, col, ang=0.0):
    for y in range(c.h):
        for x in range(c.w):
            if cov_ellipse(x, y, cx, cy, rx, ry, ang) >= 0.5:
                c.put(x, y, col, 255)


def carrot_body(c, cx, top, bot, half_top, taper=0.82, tint=0.0):
    """Tapered carrot body from `top` to `bot`, widest `half_top` at the top."""
    for y in range(c.h):
        for x in range(c.w):
            yy = y + 0.5
            if yy < top or yy > bot:
                continue
            t = (yy - top) / max(0.001, (bot - top))
            w = half_top * (1.0 - t) ** taper + 0.85
            if abs(x + 0.5 - cx) <= w:
                col = mix(ORANGE, ORANGE_D, t * 0.55)
                if tint:
                    col = mix(col, ORANGE_D, tint)
                l = (cx - (x + 0.5)) / max(1.0, half_top)
                c.put(x, y, shade(col, 1.0 + 0.13 * l), 255)


def face(c, cx, cy, scale=1.0, sleepy=False):
    """Eyes + mouth. scale shrinks the whole face for younger forms."""
    ex = 2.6 * scale
    ey = 1.35 * scale
    for y in range(c.h):
        for x in range(c.w):
            if sleepy:
                # closed eyes: short dashes
                if (abs(y + 0.5 - cy) < 0.75 * scale
                        and (abs(x + 0.5 - (cx - ex)) < 1.5 * scale
                             or abs(x + 0.5 - (cx + ex)) < 1.5 * scale)):
                    c.put(x, y, INK, 255)
            else:
                if (cov_ellipse(x, y, cx - ex, cy, 1.0 * scale, ey) >= 0.5
                        or cov_ellipse(x, y, cx + ex, cy, 1.0 * scale, ey) >= 0.5):
                    c.put(x, y, INK, 255)
    # small smile
    my = cy + 2.6 * scale
    for k in range(-1, 2):
        c.put(int(round(cx + k)), int(round(my)), INK, 255)
    c.put(int(round(cx - 2)), int(round(my - 1)), INK, 255)
    c.put(int(round(cx + 2)), int(round(my - 1)), INK, 255)


def soil(c, y0):
    """Ground line with a slightly uneven top edge."""
    bumps = {0: 0, 1: 1, 2: 0, 3: 1, 4: 0}
    for y in range(y0, c.h):
        for x in range(c.w):
            off = bumps.get(x % 5, 0)
            if y < y0 + off:
                continue
            col = SOIL if (x + y) % 3 else SOIL_D
            c.put(x, y, col, 255)


# ---------------------------------------------------------------- forms

def reference_carrot(width, height, x, y):
    """Place the authorized round Bubu reference on a 48x48 form canvas."""
    if not os.path.exists(REFERENCE_PATH):
        raise SystemExit("missing reference artwork: %s" % REFERENCE_PATH)
    source = Image.open(REFERENCE_PATH).convert("RGBA")
    sprite = source.resize((width, height), RESAMPLE_LANCZOS)
    stage = Image.new("RGBA", (FW, FH))
    stage.alpha_composite(sprite, (x, y))
    return Canvas.from_image(stage)


def form_seed():
    """0 seed: tiny round Bubu, still mostly buried."""
    c = reference_carrot(16, 18, 16, 17)
    soil(c, 32)
    return c, (32, 18)


def form_sprout():
    """1 sprout: a larger Bubu emerging from the soil."""
    c = reference_carrot(22, 24, 13, 12)
    soil(c, 34)
    return c, (32, 14)


def form_young():
    """2 young: the reference character at an intermediate scale."""
    c = reference_carrot(30, 33, 9, 10)
    return c, (32, 8)


def form_bubu():
    """3 bubu: the standard round orange reference character."""
    c = reference_carrot(42, 46, 3, 2)
    return c, (32, 4)


def form_traveler():
    """4 traveler: bubu + scarf + small backpack."""
    c, _ = form_bubu()
    fill_ellipse(c, 38.0, 34.0, 4.2, 5.2, PACK)
    for y in range(FH):
        for x in range(FW):
            if cov_ellipse(x, y, 38.0, 34.0, 4.2, 5.2) >= 0.5:
                if abs(y + 0.5 - 34.0) < 1.1:
                    c.put(x, y, shade(PACK, 0.72), 255)
    for x in range(32, 38):
        c.put(x, 31, shade(PACK, 0.8), 255)
    for x in range(9, 38):
        for y in (37, 38):
            if c.alpha(x, y):
                c.put(x, y, SCARF if (x + y) % 4 else shade(SCARF, 0.82), 255)
    for y in range(39, 45):
        for x in range(10, 13):
            c.put(x, y, SCARF if y % 2 else shade(SCARF, 0.8), 255)
    return c, (32, 4)


def form_world():
    """5 world: traveler + a halo of small stars."""
    c, _ = form_traveler()
    for ang in (-58, -20, 20, 58):
        r = math.radians(ang)
        sx = 24.0 + 13.6 * math.sin(r)
        sy = 6.6 - 5.0 * math.cos(r)
        ix, iy = int(round(sx)), int(round(sy))
        c.put(ix, iy, STAR, 255)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            c.put(ix + dx, iy + dy, mix(STAR, WHITE, 0.35), 255)
    return c, (32, 4)


# ---------------------------------------------------------------- accessories

def acc_pillow():
    """0 homebody: a pillow."""
    c = Canvas(AW, AH)
    body = (196, 206, 232)
    for y in range(4, 13):
        for x in range(2, 14):
            edge = x in (2, 13) or y in (4, 12)
            c.put(x, y, shade(body, 0.78) if edge else body, 255)
    for x in range(4, 12):
        c.put(x, 8, shade(body, 0.86), 255)
    c.outline((92, 100, 124))
    return c


def acc_dust():
    """1 rover: dust puffs."""
    c = Canvas(AW, AH)
    d = (198, 186, 166)
    for cx, cy, r in ((4.5, 10.5, 3.0), (9.0, 8.4, 2.4), (12.6, 11.0, 2.0)):
        for y in range(AH):
            for x in range(AW):
                if cov_ellipse(x, y, cx, cy, r, r * 0.78) >= 0.5:
                    c.put(x, y, d, 255)
    for y in range(AH):
        for x in range(AW):
            if c.alpha(x, y):
                if cov_ellipse(x, y, 4.5, 10.5, 1.6, 1.2) >= 0.5:
                    c.put(x, y, mix(d, WHITE, 0.5), 255)
    c.outline((140, 130, 112))
    return c


def acc_hearts():
    """2 social: two floating hearts."""
    c = Canvas(AW, AH)

    def heart(cx, cy, s, col):
        for y in range(AH):
            for x in range(AW):
                px, py = (x + 0.5 - cx) / s, (y + 0.5 - cy) / s
                if py < 0:
                    if (cov_ellipse(x, y, cx - 0.55 * s, cy - 0.35 * s, 0.62 * s, 0.62 * s) >= 0.5
                            or cov_ellipse(x, y, cx + 0.55 * s, cy - 0.35 * s, 0.62 * s, 0.62 * s) >= 0.5):
                        c.put(x, y, col, 255)
                else:
                    if abs(px) <= (1.15 - py * 0.95) and py <= 1.15:
                        c.put(x, y, col, 255)

    heart(5.4, 9.4, 3.0, PINKC)
    heart(11.0, 5.4, 2.2, mix(PINKC, WHITE, 0.28))
    c.outline((196, 88, 130))
    return c


def acc_quiet():
    """3 solo: a single quiet dot with a thin ring."""
    c = Canvas(AW, AH)
    g = (150, 152, 158)
    for y in range(AH):
        for x in range(AW):
            d = math.hypot(x + 0.5 - 8.0, y + 0.5 - 8.0)
            if 4.4 <= d <= 5.4:
                c.put(x, y, mix(g, PAPER, 0.35), 255)
            elif d <= 2.4:
                c.put(x, y, g, 255)
    return c


def acc_glowstick():
    """4 arena: a mini glow stick, lit."""
    c = Canvas(AW, AH)
    stick = (72, 78, 96)
    glow = (120, 214, 255)
    for y in range(9, 15):
        for x in range(6, 9):
            c.put(x, y, stick if x != 6 else shade(stick, 0.8), 255)
    for y in range(2, 10):
        for x in range(5, 10):
            edge = x in (5, 9)
            c.put(x, y, mix(glow, WHITE, 0.55) if edge else glow, 255)
    for y in range(3, 8):
        c.put(7, y, mix(glow, WHITE, 0.85), 255)
    for dx, dy in ((3, 1), (11, 1), (2, 5), (12, 4)):
        c.put(dx, dy, mix(glow, WHITE, 0.4), 255)
    c.outline((40, 46, 64), skip=lambda x, y: y < 2)
    return c


# ---------------------------------------------------------------- export

FORMS = [
    ("seed", "PET_FORM_SEED", 0, form_seed),
    ("sprout", "PET_FORM_SPROUT", 2, form_sprout),
    ("young", "PET_FORM_YOUNG", 4, form_young),
    ("bubu", "PET_FORM_BUBU", 8, form_bubu),
    ("traveler", "PET_FORM_TRAVELER", 12, form_traveler),
    ("world", "PET_FORM_WORLD", 16, form_world),
]

ACCS = [
    ("pillow", "PET_TRAIT_HOMEBODY", acc_pillow),
    ("dust", "PET_TRAIT_ROVER", acc_dust),
    ("hearts", "PET_TRAIT_SOCIAL", acc_hearts),
    ("quiet", "PET_TRAIT_SOLO", acc_quiet),
    ("glowstick", "PET_TRAIT_ARENA", acc_glowstick),
]


def to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def emit_array(lines, sym, im):
    px = list(im.getdata())
    cb, ab = [], []
    for (r, g, b, a) in px:
        v = to565(r, g, b)
        cb.append(v & 0xFF)
        cb.append((v >> 8) & 0xFF)
        ab.append(a)
    data = cb + ab
    lines.append("static const uint8_t %s_map[] = {" % sym)
    for i in range(0, len(data), 16):
        lines.append("    %s," % ", ".join("0x%02X" % b for b in data[i:i + 16]))
    lines.append("};")
    lines.append("")
    w, h = im.size
    lines.append("const lv_image_dsc_t %s = {" % sym)
    lines.append("    .header = {")
    lines.append("        .magic  = LV_IMAGE_HEADER_MAGIC,")
    lines.append("        .cf     = LV_COLOR_FORMAT_RGB565A8,")
    lines.append("        .flags  = 0,")
    lines.append("        .w      = %d," % w)
    lines.append("        .h      = %d," % h)
    lines.append("        .stride = %d," % (w * 2))
    lines.append("    },")
    lines.append("    .data_size = sizeof(%s_map)," % sym)
    lines.append("    .data      = %s_map," % sym)
    lines.append("};")
    lines.append("")


def emit(forms, accs, anchors):
    lines = [
        "// Auto-generated by tools/gen_pet.py -- do not edit by hand.",
        "// Pet sprites: 6 forms (48x48) + 5 personality accessories (16x16).",
        "// LVGL LV_COLOR_FORMAT_RGB565A8: w*h RGB565 LE pixels, then w*h A8 bytes.",
        "",
        '#include "pet_sprites.h"',
        "",
    ]
    for name, _, _, _ in FORMS:
        emit_array(lines, "pet_form_%s" % name, forms[name])
    for name, _, _ in ACCS:
        emit_array(lines, "pet_acc_%s" % name, accs[name])

    lines.append("const lv_image_dsc_t *const pet_form_sprites[PET_FORM_COUNT] = {")
    for i, (name, _, _, _) in enumerate(FORMS):
        lines.append("    [%d] = &pet_form_%s," % (i, name))
    lines.append("};")
    lines.append("")
    lines.append("const lv_image_dsc_t *const pet_acc_sprites[PET_TRAIT_COUNT] = {")
    for i, (name, _, _) in enumerate(ACCS):
        lines.append("    [%d] = &pet_acc_%s," % (i, name))
    lines.append("};")
    lines.append("")
    lines.append("// Where to blit a 16x16 accessory inside the 48x48 form.")
    lines.append("// Chosen so the accessory never covers the face.")
    lines.append("const pet_anchor_t pet_form_anchors[PET_FORM_COUNT] = {")
    for i, (name, _, _, _) in enumerate(FORMS):
        ax, ay = anchors[name]
        lines.append("    [%d] = { .x = %d, .y = %d }," % (i, ax, ay))
    lines.append("};")
    lines.append("")
    lines.append("// Minimum discovered-place count required for each form.")
    lines.append("const uint8_t pet_form_threshold[PET_FORM_COUNT] = {")
    for i, (_, _, thr, _) in enumerate(FORMS):
        lines.append("    [%d] = %d," % (i, thr))
    lines.append("};")
    lines.append("")
    lines.append("uint8_t pet_form_for_places(uint8_t places)")
    lines.append("{")
    lines.append("    uint8_t form = 0;")
    lines.append("    for (uint8_t i = 0; i < PET_FORM_COUNT; i++) {")
    lines.append("        if (places >= pet_form_threshold[i]) {")
    lines.append("            form = i;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return form; // monotonic: callers must never decrease a stored form")
    lines.append("}")
    lines.append("")
    with open(os.path.join(ROOT, "pet_sprites.c"), "w") as f:
        f.write("\n".join(lines))

    h = [
        "// Auto-generated by tools/gen_pet.py -- do not edit by hand.",
        "#ifndef PET_SPRITES_H",
        "#define PET_SPRITES_H",
        "",
        '#include "lvgl.h"',
        "",
        "#define PET_FORM_W %d" % FW,
        "#define PET_FORM_H %d" % FH,
        "#define PET_ACC_W %d" % AW,
        "#define PET_ACC_H %d" % AH,
        "#define PET_FORM_COUNT %d" % len(FORMS),
        "#define PET_TRAIT_COUNT %d" % len(ACCS),
        "",
        "// Evolution is driven ONLY by the number of discovered places.",
        "// Forms never regress: no death, no decay, no daily care.",
        "enum {",
    ]
    for i, (_, sym, thr, _) in enumerate(FORMS):
        h.append("    %s = %d, // >= %d places" % (sym, i, thr))
    h += ["};", "", "// Personality accessories composite on top of a form.", "enum {"]
    for i, (_, sym, _) in enumerate(ACCS):
        h.append("    %s = %d," % (sym, i))
    h += [
        "};",
        "",
        "typedef struct {",
        "    int16_t x;",
        "    int16_t y;",
        "} pet_anchor_t;",
        "",
    ]
    for name, _, _, _ in FORMS:
        h.append("extern const lv_image_dsc_t pet_form_%s;" % name)
    for name, _, _ in ACCS:
        h.append("extern const lv_image_dsc_t pet_acc_%s;" % name)
    h += [
        "",
        "extern const lv_image_dsc_t *const pet_form_sprites[PET_FORM_COUNT];",
        "extern const lv_image_dsc_t *const pet_acc_sprites[PET_TRAIT_COUNT];",
        "extern const pet_anchor_t pet_form_anchors[PET_FORM_COUNT];",
        "extern const uint8_t pet_form_threshold[PET_FORM_COUNT];",
        "",
        "// Returns the form index for a given place count. Pure function, testable",
        "// without hardware. Callers MUST clamp so a stored form never decreases.",
        "uint8_t pet_form_for_places(uint8_t places);",
        "",
        "#endif // PET_SPRITES_H",
        "",
    ]
    with open(os.path.join(ROOT, "pet_sprites.h"), "w") as f:
        f.write("\n".join(h))


def pick_font(size):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def previews(forms, accs, anchors):
    fl = ["0 SEED >=0", "1 SPROUT >=2", "2 YOUNG >=4",
          "3 BUBU >=8", "4 TRAVELER >=12", "5 WORLD >=16"]
    al = ["0 HOMEBODY", "1 ROVER", "2 SOCIAL", "3 SOLO", "4 ARENA"]

    # forms 1x + scaled grid
    gap = 4
    strip = Image.new("RGB", (6 * FW + 7 * gap, FH + 2 * gap), PAPER)
    for i, (name, _, _, _) in enumerate(FORMS):
        strip.paste(forms[name], (gap + i * (FW + gap), gap), forms[name])
    strip.save(os.path.join(PREV_DIR, "forms_1x.png"))
    strip.resize((strip.width * 4, strip.height * 4), Image.NEAREST).save(
        os.path.join(PREV_DIR, "forms_x4.png"))

    sc = 6
    cw, ch = FW * sc + 20, FH * sc + 46
    for bg, fg, fn in ((PAPER, (40, 38, 34), "forms_light_x6.png"),
                       (DARK, (232, 230, 226), "forms_dark_x6.png")):
        sheet = Image.new("RGB", (cw * 3, ch * 2), bg)
        dr = ImageDraw.Draw(sheet)
        ft = pick_font(19)
        for i, (name, _, _, _) in enumerate(FORMS):
            r, cc = divmod(i, 3)
            ox, oy = cc * cw + 10, r * ch + 10
            big = forms[name].resize((FW * sc, FH * sc), Image.NEAREST)
            sheet.paste(big, (ox, oy), big)
            dr.rectangle([ox - 1, oy - 1, ox + FW * sc, oy + FH * sc],
                         outline=(150, 146, 138))
            dr.text((ox, oy + FH * sc + 8), fl[i], fill=fg, font=ft)
        sheet.save(os.path.join(PREV_DIR, fn))

    # accessories
    sc = 8
    cw, ch = AW * sc + 20, AH * sc + 46
    sheet = Image.new("RGB", (cw * 5, ch), PAPER)
    dr = ImageDraw.Draw(sheet)
    ft = pick_font(17)
    for i, (name, _, _) in enumerate(ACCS):
        ox, oy = i * cw + 10, 10
        big = accs[name].resize((AW * sc, AH * sc), Image.NEAREST)
        sheet.paste(big, (ox, oy), big)
        dr.rectangle([ox - 1, oy - 1, ox + AW * sc, oy + AH * sc],
                     outline=(150, 146, 138))
        dr.text((ox, oy + AH * sc + 8), al[i], fill=(40, 38, 34), font=ft)
    sheet.save(os.path.join(PREV_DIR, "accessories_x8.png"))

    # composite matrix: every form x every accessory, at the published anchor
    sc = 3
    cw, ch = FW * sc + 12, FH * sc + 30
    sheet = Image.new("RGB", (cw * 6 + 90, ch * 6), PAPER)
    dr = ImageDraw.Draw(sheet)
    ft = pick_font(15)
    ftl = pick_font(14)
    for col, (fname, _, _, _) in enumerate(FORMS):
        dr.text((90 + col * cw + 6, 6), fl[col].split()[1], fill=(40, 38, 34), font=ftl)
    for row in range(6):
        label = "(none)" if row == 0 else al[row - 1].split(" ", 1)[1]
        dr.text((6, 30 + row * ch + FH * sc // 2), label, fill=(40, 38, 34), font=ftl)
        for col, (fname, _, _, _) in enumerate(FORMS):
            base = forms[fname].copy()
            if row > 0:
                aname = ACCS[row - 1][0]
                ax, ay = anchors[fname]
                base.alpha_composite(accs[aname], (ax, ay))
            big = base.resize((FW * sc, FH * sc), Image.NEAREST)
            ox, oy = 90 + col * cw + 6, 30 + row * ch
            sheet.paste(big, (ox, oy), big)
            dr.rectangle([ox - 1, oy - 1, ox + FW * sc, oy + FH * sc],
                         outline=(206, 200, 186))
    sheet.save(os.path.join(PREV_DIR, "composite_matrix.png"))


def main():
    os.makedirs(PNG_DIR, exist_ok=True)
    os.makedirs(PREV_DIR, exist_ok=True)

    forms, anchors = {}, {}
    for i, (name, _, thr, fn) in enumerate(FORMS):
        c, anc = fn()
        im = c.to_image()
        im.save(os.path.join(PNG_DIR, "form_%d_%s.png" % (i, name)))
        forms[name] = im
        anchors[name] = anc

    # An anchor must keep a whole 16x16 accessory inside the 48x48 canvas.
    # Pillow's alpha_composite silently CROPS an out-of-range blit, so a bad
    # anchor still looks fine in the preview and only clips on the device.
    # Fail loudly at generation time instead.
    bad = []
    for _n, (ax, ay) in anchors.items():
        if not (0 <= ax and ax + AW <= FW and 0 <= ay and ay + AH <= FH):
            bad.append("%-9s anchor=(%2d,%2d) -> right=%d bottom=%d, canvas %dx%d"
                       % (_n, ax, ay, ax + AW, ay + AH, FW, FH))
    if bad:
        raise SystemExit("ANCHOR OUT OF BOUNDS:\n  " + "\n  ".join(bad))

    accs = {}
    for i, (name, _, fn) in enumerate(ACCS):
        im = fn().to_image()
        im.save(os.path.join(PNG_DIR, "acc_%d_%s.png" % (i, name)))
        accs[name] = im


    # The accessory must not cover the face. Locate Bubu's blue eyes, then
    # expand that box to include the mouth below them.
    overlaps = []
    for _fn, (ax, ay) in anchors.items():
        fim = forms[_fn]
        eyes = [(x, y) for y in range(FH) for x in range(FW)
                if fim.getpixel((x, y))[3] > 0
                and fim.getpixel((x, y))[2] > 90
                and fim.getpixel((x, y))[2] > fim.getpixel((x, y))[0] * 1.25
                and fim.getpixel((x, y))[2] > fim.getpixel((x, y))[1] * 1.10]
        if not eyes:
            overlaps.append("%s: reference eye pixels not found" % _fn)
            continue
        fx0 = max(0, min(p[0] for p in eyes) - 3)
        fx1 = min(FW - 1, max(p[0] for p in eyes) + 3)
        fy0 = max(0, min(p[1] for p in eyes) - 2)
        fy1 = min(FH - 1, max(p[1] for p in eyes) + 7)
        for _an, aim in accs.items():
            hits = sum(1 for y in range(AH) for x in range(AW)
                       if aim.getpixel((x, y))[3] > 0
                       and fx0 <= ax + x <= fx1 and fy0 <= ay + y <= fy1)
            if hits:
                overlaps.append("%s + %s: %d px inside face box x[%d..%d] y[%d..%d]"
                                % (_fn, _an, hits, fx0, fx1, fy0, fy1))
    if overlaps:
        raise SystemExit("ACCESSORY COVERS FACE:\n  " + "\n  ".join(overlaps))

    emit(forms, accs, anchors)
    previews(forms, accs, anchors)

    print("FORMS (48x48)      opaque  anchor")
    for i, (name, _, thr, _) in enumerate(FORMS):
        op = sum(1 for p in forms[name].getdata() if p[3] > 0)
        print("  %d %-10s >=%2d  %5d  %s" % (i, name, thr, op, anchors[name]))
    print("ACCESSORIES (16x16)  opaque")
    for i, (name, _, _) in enumerate(ACCS):
        op = sum(1 for p in accs[name].getdata() if p[3] > 0)
        print("  %d %-10s      %4d" % (i, name, op))
    fb, ab = FW * FH * 3, AW * AH * 3
    print("\nFlash: forms %d B x6 = %d B (%.1f KB)" % (fb, fb * 6, fb * 6 / 1024))
    print("       accs  %d B x5 = %d B (%.1f KB)" % (ab, ab * 5, ab * 5 / 1024))
    print("       pet total = %d B (%.1f KB)"
          % (fb * 6 + ab * 5, (fb * 6 + ab * 5) / 1024))
    print("       + icon pack 24576 B => grand total %d B (%.1f KB)"
          % (fb * 6 + ab * 5 + 24576, (fb * 6 + ab * 5 + 24576) / 1024))


if __name__ == "__main__":
    main()
