#!/usr/bin/env python3
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/community-publish/cover.png"
W, H = 1152, 1536

FONT_ZH = "/System/Library/Fonts/Hiragino Sans GB.ttc"
FONT_LATIN = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"


def font(path, size, index=0):
    return ImageFont.truetype(path, size=size, index=index)


def centered(draw, xy, text, text_font, fill, stroke=0, stroke_fill=None):
    x, y = xy
    box = draw.textbbox((0, 0), text, font=text_font, stroke_width=stroke)
    draw.text(
        (x - (box[2] - box[0]) / 2, y),
        text,
        font=text_font,
        fill=fill,
        stroke_width=stroke,
        stroke_fill=stroke_fill,
    )


def sprite(path, scale):
    image = Image.open(path).convert("RGBA")
    return image.resize(
        (image.width * scale, image.height * scale), Image.Resampling.NEAREST
    )


def paste_shadow(canvas, image, xy, radius=18, offset=(10, 16)):
    alpha = image.getchannel("A")
    shadow = Image.new("RGBA", image.size, (18, 41, 61, 0))
    shadow.putalpha(alpha.filter(ImageFilter.GaussianBlur(radius)))
    canvas.alpha_composite(shadow, (xy[0] + offset[0], xy[1] + offset[1]))
    canvas.alpha_composite(image, xy)


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    canvas = Image.new("RGBA", (W, H), "#168FE3")
    draw = ImageDraw.Draw(canvas)

    # Sky, distant skyline, and grass keep the world close to the firmware UI.
    draw.rectangle((0, 0, W, 1010), fill="#168FE3")
    for x, y, ww in ((70, 242, 180), (850, 294, 210), (420, 360, 150)):
        draw.rounded_rectangle((x, y, x + ww, y + 52), 26, fill="#EAF8FF")
        draw.ellipse((x + 28, y - 24, x + 92, y + 48), fill="#EAF8FF")
        draw.ellipse((x + 78, y - 38, x + 152, y + 48), fill="#EAF8FF")

    draw.rectangle((0, 1010, W, H), fill="#8BCB32")
    draw.rectangle((0, 1010, W, 1036), fill="#B9EA52")
    for x in range(0, W, 96):
        draw.rectangle((x, 1036, x + 46, 1062), fill="#6AAE25")
    draw.rectangle((0, 1112, W, H), fill="#F4F0E3")

    centered(
        draw,
        (W // 2, 72),
        "卜卜通行证",
        font(FONT_ZH, 128),
        "#FFF9E7",
        stroke=5,
        stroke_fill="#15334A",
    )
    centered(
        draw,
        (W // 2, 230),
        "让 wmls 在人群里认出彼此",
        font(FONT_ZH, 45),
        "#15334A",
    )

    left = sprite(ROOT / "main/assets/png_pet/form_3_bubu.png", 6)
    right = sprite(ROOT / "main/assets/png_pet/form_5_world.png", 6)
    paste_shadow(canvas, left, (126, 505))
    paste_shadow(canvas, right, (738, 505))

    carrot = sprite(ROOT / "main/assets/png/00_carrot.png", 3)
    rabbit = sprite(ROOT / "main/assets/png/01_rabbit.png", 3)
    nine = sprite(ROOT / "main/assets/png/07_nineball.png", 3)
    canvas.alpha_composite(carrot, (226, 414))
    canvas.alpha_composite(rabbit, (830, 414))
    canvas.alpha_composite(nine, (W // 2 - 48, 720))

    # Recognition spark between the two Bubu characters.
    for radius, color, width in (
        (122, "#FFF3A6", 18),
        (82, "#FFFFFF", 14),
        (44, "#FFD23F", 12),
    ):
        draw.arc(
            (W // 2 - radius, 548 - radius, W // 2 + radius, 548 + radius),
            205,
            335,
            fill=color,
            width=width,
        )
    centered(draw, (W // 2, 850), "暗号相认，也可以安静陪伴", font(FONT_ZH, 40), "#15334A")

    draw.rounded_rectangle((54, 1146, W - 54, 1468), 28, fill="#FFFDF7")
    centered(draw, (W // 2, 1176), "带卜卜去更多地方", font(FONT_ZH, 46), "#15334A")

    form_paths = sorted((ROOT / "main/assets/png_pet").glob("form_*.png"))
    labels = ["种子", "芽", "幼卜", "卜卜", "旅卜", "世界卜"]
    x0, gap = 95, 190
    for i, (path, label) in enumerate(zip(form_paths, labels)):
        image = sprite(path, 2)
        x = x0 + i * gap
        canvas.alpha_composite(image, (x - image.width // 2, 1260))
        centered(draw, (x, 1370), label, font(FONT_ZH, 28), "#4D3A28")
        if i < len(form_paths) - 1:
            draw.polygon(
                ((x + 130, 1310), (x + 102, 1294), (x + 102, 1304),
                 (x + 70, 1304), (x + 70, 1316), (x + 102, 1316),
                 (x + 102, 1326)),
                fill="#F28A2E",
            )

    centered(
        draw,
        (W // 2, 1480),
        "认出同好 · 收藏路程 · 只长大，不掉级",
        font(FONT_ZH, 30),
        "#31561A",
    )
    canvas.convert("RGB").save(OUT, quality=95)
    print(OUT)


if __name__ == "__main__":
    main()
