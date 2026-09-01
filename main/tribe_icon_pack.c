#include "tribe_icon_pack.h"

#include "tribe_icons_mayday.h"

const tribe_icon_pack_t tribe_icon_pack_active = {
    .name = "mayday",
    .icons = tribe_icon_pack_mayday,
    .count = TRIBE_ICON_COUNT,
    .fallback_index = TRIBE_ICON_CARROT,
};

const lv_image_dsc_t *tribe_icon_get(const tribe_icon_pack_t *pack,
                                     uint8_t icon_index)
{
    if (!pack || !pack->icons || pack->count == 0) return NULL;
    if (icon_index >= pack->count) icon_index = pack->fallback_index;
    if (icon_index >= pack->count) icon_index = 0;
    return pack->icons[icon_index];
}
