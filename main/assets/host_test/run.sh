#!/usr/bin/env bash
# 不依赖 ESP-IDF / 真机的主机侧测试：验证形态映射、header 字段、anchor 边界。
# AGENTS.md 要求可测状态机与布局计算脱离 IDF/LVGL 并有 host test。
set -e
cd "$(dirname "$0")"
cp lvgl_stub.h lvgl.h
gcc -std=c11 -Wall -Wextra -Werror -I. -I.. -I../.. -o /tmp/pet_host_test \
    test_pet_sprites.c ../pet_sprites.c ../tribe_icons_mayday.c \
    ../../tribe_icon_pack.c
rm -f lvgl.h
/tmp/pet_host_test
