#ifndef LVGL_H
#define LVGL_H
#include <stdint.h>
#include <stddef.h>
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565A8 0x14
typedef struct { uint32_t magic:8; uint32_t cf:8; uint32_t flags:16;
                 uint32_t w:16; uint32_t h:16; uint32_t stride:16; } lv_image_header_t;
typedef struct { lv_image_header_t header; uint32_t data_size; const uint8_t *data; } lv_image_dsc_t;
#endif
