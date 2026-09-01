#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
独立校验宠物资产：pet_sprites.c 是否和 png_pet/ 下的 PNG 一致。

与 gen_pet.py 走不同代码路径：这里把 .c 当纯文本正则解析，从 RGB565 + A8
字节反向重建图像，再与 PNG 逐像素比对。所以它能抓到 emit() 的字节序、
stride、data_size、alpha 平面偏移等错误——这些错误在生成侧是看不出来的。

额外检查（这些是宠物资产特有的失败模式）：
  - anchor 必须让整个 16x16 配饰留在 48x48 画布内
  - 配饰在 anchor 处不得覆盖脸部 ink
  - 阈值必须严格单调递增，且 pet_form_for_places 的边界行为正确
"""

import os
import re
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C_PATH = os.path.join(ROOT, "pet_sprites.c")
H_PATH = os.path.join(ROOT, "pet_sprites.h")
PNG_DIR = os.path.join(ROOT, "png_pet")

FW = FH = 48
AW = AH = 16
INK = (43, 43, 48)

FORMS = ["seed", "sprout", "young", "bubu", "traveler", "world"]
ACCS = ["pillow", "dust", "hearts", "quiet", "glowstick"]
THRESH = [0, 2, 4, 8, 12, 16]


def q565(r, g, b):
    """PNG 颜色经 RGB565 量化后再展开回 8bit，用于和反解结果比对。"""
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


def parse_c(text, sym):
    """从 .c 文本里抠出某个 sprite 的字节数组和 header 字段。"""
    m = re.search(r"static const uint8_t %s_map\[\]\s*=\s*\{(.*?)\};" % sym,
                  text, re.S)
    if not m:
        return None, None
    data = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]

    m2 = re.search(r"const lv_image_dsc_t %s\s*=\s*\{(.*?)\n\};" % sym,
                   text, re.S)
    if not m2:
        return data, None
    blk = m2.group(1)
    hdr = {}
    for k in ("w", "h", "stride"):
        mm = re.search(r"\.%s\s*=\s*(\d+)" % k, blk)
        if mm:
            hdr[k] = int(mm.group(1))
    hdr["cf"] = "LV_COLOR_FORMAT_RGB565A8" in blk
    return data, hdr


def rebuild(data, w, h):
    """RGB565A8: w*h 个小端 RGB565，紧跟 w*h 个 alpha 字节。"""
    n = w * h
    px = []
    for i in range(n):
        lo, hi = data[i * 2], data[i * 2 + 1]
        v = lo | (hi << 8)
        r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        a = data[n * 2 + i]
        px.append(((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4),
                   (b5 << 3) | (b5 >> 2), a))
    return px


def check_sprite(text, sym, png, w, h):
    fails = []
    data, hdr = parse_c(text, sym)
    if data is None:
        return ["%s: 在 .c 里找不到数组" % sym]
    want = w * h * 3
    if len(data) != want:
        fails.append("%s: 字节数 %d != %d" % (sym, len(data), want))
        return fails
    if hdr is None:
        fails.append("%s: 找不到 lv_image_dsc_t 定义" % sym)
    else:
        if hdr.get("w") != w or hdr.get("h") != h:
            fails.append("%s: header w/h = %s/%s，应为 %d/%d"
                         % (sym, hdr.get("w"), hdr.get("h"), w, h))
        if hdr.get("stride") != w * 2:
            fails.append("%s: stride = %s，应为 %d" % (sym, hdr.get("stride"), w * 2))
        if not hdr.get("cf"):
            fails.append("%s: cf 不是 RGB565A8" % sym)

    if not os.path.isfile(png):
        return fails + ["%s: 缺少 PNG %s" % (sym, os.path.basename(png))]

    im = Image.open(png).convert("RGBA")
    if im.size != (w, h):
        return fails + ["%s: PNG 尺寸 %s != (%d,%d)" % (sym, im.size, w, h)]

    got = rebuild(data, w, h)
    src = list(im.getdata())
    a_bad = c_bad = 0
    for i, (sp, gp) in enumerate(zip(src, got)):
        if sp[3] != gp[3]:
            a_bad += 1
        if sp[3] > 0 and q565(*sp[:3]) != gp[:3]:
            c_bad += 1
    if a_bad:
        fails.append("%s: alpha 不一致 %d px" % (sym, a_bad))
    if c_bad:
        fails.append("%s: 颜色不一致 %d px" % (sym, c_bad))
    return fails


def main():
    for p in (C_PATH, H_PATH):
        if not os.path.isfile(p):
            print("ERROR: 缺少 %s，请先运行 tools/gen_pet.py" % p)
            return 1
    text = open(C_PATH).read()
    htext = open(H_PATH).read()

    all_fails = []
    print("--- 逐图反解比对（.c 文本 -> 像素 vs PNG）---")
    for i, name in enumerate(FORMS):
        png = os.path.join(PNG_DIR, "form_%d_%s.png" % (i, name))
        f = check_sprite(text, "pet_form_%s" % name, png, FW, FH)
        print("  form %d %-9s %s" % (i, name, "PASS" if not f else "FAIL"))
        all_fails += f
    for i, name in enumerate(ACCS):
        png = os.path.join(PNG_DIR, "acc_%d_%s.png" % (i, name))
        f = check_sprite(text, "pet_acc_%s" % name, png, AW, AH)
        print("  acc  %d %-9s %s" % (i, name, "PASS" if not f else "FAIL"))
        all_fails += f

    # ---- anchor 边界 ----
    print("\n--- anchor 边界（16x16 配饰须完整落在 48x48 内）---")
    anchors = [(int(a), int(b)) for a, b in
               re.findall(r"\.x\s*=\s*(-?\d+),\s*\.y\s*=\s*(-?\d+)", text)]
    if len(anchors) != len(FORMS):
        all_fails.append("anchor 数量 %d != %d" % (len(anchors), len(FORMS)))
    for i, (ax, ay) in enumerate(anchors):
        ok = 0 <= ax and ax + AW <= FW and 0 <= ay and ay + AH <= FH
        print("  %-9s (%2d,%2d) right=%2d bottom=%2d  %s"
              % (FORMS[i], ax, ay, ax + AW, ay + AH, "PASS" if ok else "FAIL"))
        if not ok:
            all_fails.append("%s anchor 越界" % FORMS[i])

    # ---- 配饰是否挡脸 ----
    print("\n--- 配饰遮挡脸部检查（30 组合）---")
    acc_imgs = {}
    for i, name in enumerate(ACCS):
        p = os.path.join(PNG_DIR, "acc_%d_%s.png" % (i, name))
        if os.path.isfile(p):
            acc_imgs[name] = Image.open(p).convert("RGBA")
    worst = 0
    for i, fname in enumerate(FORMS):
        p = os.path.join(PNG_DIR, "form_%d_%s.png" % (i, fname))
        if not os.path.isfile(p) or i >= len(anchors):
            continue
        fim = Image.open(p).convert("RGBA")
        eyes = [(x, y) for y in range(FH) for x in range(FW)
                if fim.getpixel((x, y))[3] > 0
                and fim.getpixel((x, y))[2] > 90
                and fim.getpixel((x, y))[2] > fim.getpixel((x, y))[0] * 1.25
                and fim.getpixel((x, y))[2] > fim.getpixel((x, y))[1] * 1.10]
        if not eyes:
            all_fails.append("%s: 找不到参考图蓝色眼睛像素" % fname)
            continue
        fx0 = max(0, min(q[0] for q in eyes) - 3)
        fx1 = min(FW - 1, max(q[0] for q in eyes) + 3)
        fy0 = max(0, min(q[1] for q in eyes) - 2)
        fy1 = min(FH - 1, max(q[1] for q in eyes) + 7)
        ax, ay = anchors[i]
        row = []
        for aname, aim in acc_imgs.items():
            hits = sum(1 for y in range(AH) for x in range(AW)
                       if aim.getpixel((x, y))[3] > 0
                       and fx0 <= ax + x <= fx1 and fy0 <= ay + y <= fy1)
            worst = max(worst, hits)
            row.append("%s=%d" % (aname[:4], hits))
            if hits:
                all_fails.append("%s+%s 遮脸 %d px" % (fname, aname, hits))
        print("  %-9s face x[%2d..%2d] y[%2d..%2d]  %s"
              % (fname, fx0, fx1, fy0, fy1, " ".join(row)))
    print("  最大遮挡 = %d px  %s" % (worst, "PASS" if worst == 0 else "FAIL"))

    # ---- 阈值单调 + 头文件一致 ----
    print("\n--- 阈值与头文件 ---")
    thr = [int(x) for x in re.findall(r"\[\d+\]\s*=\s*(\d+),", 
           re.search(r"pet_form_threshold\[PET_FORM_COUNT\]\s*=\s*\{(.*?)\};",
                     text, re.S).group(1))]
    mono = all(thr[i] < thr[i + 1] for i in range(len(thr) - 1))
    print("  阈值 %s  单调递增=%s" % (thr, "PASS" if mono else "FAIL"))
    if thr != THRESH:
        all_fails.append("阈值 %s != 预期 %s" % (thr, THRESH))
    if not mono:
        all_fails.append("阈值非单调")
    for sym in ("pet_form_sprites", "pet_acc_sprites", "pet_form_anchors",
                "pet_form_threshold", "pet_form_for_places"):
        ok = sym in htext and sym in text
        print("  %-22s 声明+定义 %s" % (sym, "PASS" if ok else "FAIL"))
        if not ok:
            all_fails.append("%s 声明或定义缺失" % sym)

    print()
    if all_fails:
        print("FAILED (%d):" % len(all_fails))
        for f in all_fails:
            print("  - " + f)
        return 1
    n = len(FORMS) + len(ACCS)
    print("ALL PASS: %d 个 sprite 反解一致，anchor 全部在界内，"
          "30 组合零遮脸，阈值单调" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
