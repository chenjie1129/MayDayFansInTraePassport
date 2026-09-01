#pragma once

#include "lvgl.h"

#include <stdint.h>

#define TRIBE_ICON_PACK_COUNT 8u

typedef struct {
    const char *name;
    const lv_image_dsc_t *const *icons;
    uint8_t count;
    uint8_t fallback_index;
} tribe_icon_pack_t;

extern const tribe_icon_pack_t tribe_icon_pack_active;

const lv_image_dsc_t *tribe_icon_get(const tribe_icon_pack_t *pack,
                                     uint8_t icon_index);
