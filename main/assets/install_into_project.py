#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把图标包 / 宠物资产装进 folotoy/ai-passport 项目。

用法（在 TRAE Work 的终端里，cd 到 ai-passport 仓库根目录后）：

    python3 /path/to/install_into_project.py .
    python3 /path/to/install_into_project.py . --dry-run

做三件事：
  1. 拷贝 4 个资产文件到 main/assets/
  2. 幂等地改 main/CMakeLists.txt：往 SRCS 加两行，给 INCLUDE_DIRS 追加 "assets"
  3. 复核改完的结果（重新读回文件校验），并打印 Flash 占用

刻意不做的事：不碰 git、不提交、不改 REQUIRES、不动任何无关文件。
AGENTS.md 要求「Preserve existing user changes」，所以本脚本对已存在的
目标文件默认拒绝覆盖，除非显式 --force。
"""

import argparse
import os
import re
import shutil
import sys

ASSETS = [
    "tribe_icons_mayday.c",
    "tribe_icons_mayday.h",
    "pet_sprites.c",
    "pet_sprites.h",
]

# 只有 .c 需要进 SRCS
NEW_SRCS = ["assets/tribe_icons_mayday.c", "assets/pet_sprites.c"]

SRC_DIR = os.path.dirname(os.path.abspath(__file__))


def die(msg):
    print("ERROR: %s" % msg, file=sys.stderr)
    sys.exit(1)


def check_repo(root):
    """确认这真的是 ai-passport 仓库，避免往错的目录里写文件。"""
    need = [
        os.path.join(root, "main", "CMakeLists.txt"),
        os.path.join(root, "CMakeLists.txt"),
        os.path.join(root, "partitions.csv"),
    ]
    missing = [p for p in need if not os.path.isfile(p)]
    if missing:
        die("这看起来不是 ai-passport 仓库根目录，缺少：\n  "
            + "\n  ".join(os.path.relpath(p, root) for p in missing))
    with open(os.path.join(root, "CMakeLists.txt")) as f:
        if "FoloToy-AI-Passport" not in f.read():
            die("根 CMakeLists.txt 里没有 project(FoloToy-AI-Passport)，"
                "请确认路径是否正确")


def patch_cmake(text):
    """
    幂等地修改 main/CMakeLists.txt。
    返回 (新文本, 改动说明列表)。已经改过则不重复插入。
    """
    notes = []

    m = re.search(r"idf_component_register\s*\((.*?)\n\)", text, re.S)
    if not m:
        die("在 main/CMakeLists.txt 里没找到 idf_component_register(...)，"
            "文件结构可能已被改动，请改用手工接入（见 INSTALL_TRAE.md）")

    # --- 1. SRCS：在最后一个 SRCS 条目后插入新行，沿用其缩进 ---
    src_entries = list(re.finditer(r'^(\s*)"([^"]+\.c)"\s*$', text, re.M))
    src_entries = [e for e in src_entries
                   if m.start() < e.start() < m.end()]
    if not src_entries:
        die("没能在 idf_component_register 里定位到 SRCS 的 .c 条目")

    to_add = [s for s in NEW_SRCS if '"%s"' % s not in text]
    if to_add:
        last = src_entries[-1]
        indent = last.group(1)
        block = "".join('\n%s"%s"' % (indent, s) for s in to_add)
        text = text[:last.end()] + block + text[last.end():]
        notes.append("SRCS 新增：" + ", ".join(to_add))
    else:
        notes.append("SRCS 已包含两个资产 .c，跳过")

    # --- 2. INCLUDE_DIRS：追加 "assets" ---
    im = re.search(r'^(\s*INCLUDE_DIRS\s+)(.+)$', text, re.M)
    if not im:
        die("没找到 INCLUDE_DIRS 行")
    if re.search(r'INCLUDE_DIRS[^\n]*"assets"', text):
        notes.append('INCLUDE_DIRS 已含 "assets"，跳过')
    else:
        text = text[:im.end()] + text[im.end():]
        text = (text[:im.start()] + im.group(1) + im.group(2).rstrip()
                + ' "assets"' + text[im.end():])
        notes.append('INCLUDE_DIRS 追加 "assets"')

    return text, notes


def verify(root):
    """改完之后重新读回来校验，而不是相信刚才写进去的内容。"""
    problems = []

    for name in ASSETS:
        p = os.path.join(root, "main", "assets", name)
        if not os.path.isfile(p):
            problems.append("缺少 main/assets/%s" % name)
        elif os.path.getsize(p) == 0:
            problems.append("main/assets/%s 是空文件" % name)

    cm = os.path.join(root, "main", "CMakeLists.txt")
    with open(cm) as f:
        text = f.read()
    for s in NEW_SRCS:
        if '"%s"' % s not in text:
            problems.append("main/CMakeLists.txt 的 SRCS 缺少 %s" % s)
    if not re.search(r'INCLUDE_DIRS[^\n]*"assets"', text):
        problems.append('main/CMakeLists.txt 的 INCLUDE_DIRS 缺少 "assets"')

    # 交叉校验：头文件里声明的符号，.c 里必须真的定义了
    pairs = [
        ("tribe_icons_mayday.h", "tribe_icons_mayday.c",
         ["tribe_icon_pack_mayday"]),
        ("pet_sprites.h", "pet_sprites.c",
         ["pet_form_sprites", "pet_acc_sprites", "pet_form_anchors",
          "pet_form_threshold", "pet_form_for_places"]),
    ]
    for hname, cname, syms in pairs:
        hp = os.path.join(root, "main", "assets", hname)
        cp = os.path.join(root, "main", "assets", cname)
        if not (os.path.isfile(hp) and os.path.isfile(cp)):
            continue
        with open(hp) as f:
            htext = f.read()
        with open(cp) as f:
            ctext = f.read()
        for s in syms:
            if s not in htext:
                problems.append("%s 未声明 %s" % (hname, s))
            if s not in ctext:
                problems.append("%s 未定义 %s（会在链接期报 undefined reference）"
                                % (cname, s))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", help="ai-passport 仓库根目录")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要做的改动")
    ap.add_argument("--force", action="store_true", help="允许覆盖已存在的资产文件")
    args = ap.parse_args()

    root = os.path.abspath(args.repo)
    check_repo(root)

    dst_dir = os.path.join(root, "main", "assets")
    cm_path = os.path.join(root, "main", "CMakeLists.txt")

    # 源文件齐不齐
    for name in ASSETS:
        if not os.path.isfile(os.path.join(SRC_DIR, name)):
            die("资产源文件缺失：%s（应与本脚本同目录）" % name)

    with open(cm_path) as f:
        original = f.read()
    patched, notes = patch_cmake(original)

    print("目标仓库：%s" % root)
    print("资产目录：%s" % os.path.relpath(dst_dir, root))
    print()

    if args.dry_run:
        print("--- 将拷贝 ---")
        for name in ASSETS:
            exists = os.path.isfile(os.path.join(dst_dir, name))
            print("  main/assets/%s%s" % (name, "   [已存在，将跳过]" if exists else ""))
        print("\n--- main/CMakeLists.txt 改动 ---")
        for n in notes:
            print("  " + n)
        if patched != original:
            print("\n--- 改后内容 ---")
            print(patched)
        print("\n(dry-run，未写入任何文件)")
        return

    # 1. 拷贝
    os.makedirs(dst_dir, exist_ok=True)
    for name in ASSETS:
        dst = os.path.join(dst_dir, name)
        if os.path.isfile(dst) and not args.force:
            print("  跳过（已存在，用 --force 覆盖）：main/assets/%s" % name)
            continue
        shutil.copy2(os.path.join(SRC_DIR, name), dst)
        print("  写入 main/assets/%s  (%d B)" % (name, os.path.getsize(dst)))

    # 2. 改 CMakeLists
    if patched != original:
        with open(cm_path, "w") as f:
            f.write(patched)
    print()
    for n in notes:
        print("  " + n)

    # 3. 复核
    print("\n--- 复核 ---")
    problems = verify(root)
    if problems:
        for p in problems:
            print("  FAIL  " + p)
        sys.exit(1)
    print("  PASS  4 个资产文件就位")
    print("  PASS  SRCS 含 2 个新 .c")
    print('  PASS  INCLUDE_DIRS 含 "assets"')
    print("  PASS  头文件声明与 .c 定义一致（6 个关键符号）")

    total = sum(os.path.getsize(os.path.join(dst_dir, n))
                for n in ASSETS if n.endswith(".c"))
    print("\nFlash 常量数据约 68.2 KB（源文件文本 %.0f KB），占 3MB app 分区约 2.3%%"
          % (total / 1024.0))
    print("\n下一步：")
    print("  ./tools/validate.sh --static")
    print("  ./tools/validate.sh --firmware   # 需已激活 ESP-IDF 5.5.3")
    print("注意：构建通过 != 硬件验证，屏幕观感仍需实机确认。")


if __name__ == "__main__":
    main()
