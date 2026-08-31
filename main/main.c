// main/main.c —— FoloToy AI Passport BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* [诊断] 心跳任务: 每 1s 打印一次。
 * 用途: 启动卡死时区分故障类型 —— 心跳停在某行 = 硬停(关中断死循环/cache stall);
 * 心跳继续而 main marker 停住 = 单任务阻塞(信号量/总线等待)。 */
static void hb_task(void *arg) {
    (void)arg;
    for (int n = 1; ; n++) {
        ESP_LOGI(TAG, "HB %d", n);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static const demo_entry_t DEMOS[] = {
    { "Display", demo_display_enter, demo_display_exit, demo_display_key },
    { "Button",  demo_button_enter,  demo_button_exit,  demo_button_key  },
    { "Audio",   demo_audio_enter,   demo_audio_exit,   demo_audio_key   },
    { "Battery", demo_battery_enter, demo_battery_exit, demo_battery_key },
    { "Wi-Fi",   demo_wifi_enter,    demo_wifi_exit,    demo_wifi_key    },
    { "BLE",     demo_ble_enter,     demo_ble_exit,     demo_ble_key     },
    { "Low Power", demo_low_power_enter, demo_low_power_exit, demo_low_power_key },
    { "WoodFish", demo_woodfish_enter, demo_woodfish_exit, demo_woodfish_key },
    { "Answer",  demo_answer_enter,  demo_answer_exit,  demo_answer_key  },
    { "Places",  demo_place_enter,   demo_place_exit,   demo_place_key   },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))

// 各外设初始化结果:失败的项在菜单里标 [FAIL] 且不允许进入。
static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static int  s_sel;                 // 当前选中项
static int  s_active = -1;         // 当前所在演示页;-1 = 在菜单

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name,
                              s_ok[i] ? "" : "  [FAIL]");
        ui_pixel_set_selected(s_cards[i], (int)i == s_sel, s_ok[i]);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[i] ? lv_color_hex(UI_INK) : lv_color_hex(0x7A2020), 0);
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("FoloToy");

    // 2 列 x 5 行:卡片高 38、行距 45(投影 +6 恰好不压下一行),
    // 底行 y=228..266,吉祥物固定站在草地 y=272..320,互不重叠。
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int x = 11 + (int)(i % 2) * 112;
        int y = 48 + (int)(i / 2) * 45;
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, x, y, 102, 38, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 272);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    s_active = -1;
    menu_build();
}

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    if (s_active >= 0) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {     // 统一返回
            DEMOS[s_active].exit();
            enter_menu();
        } else {
            DEMOS[s_active].key(btn, ev);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP)   { s_sel = (s_sel + DEMO_COUNT - 1) % DEMO_COUNT; menu_refresh(); }
        if (btn == BSP_BTN_DOWN) { s_sel = (s_sel + 1) % DEMO_COUNT;              menu_refresh(); }
        if (btn == BSP_BTN_OK && s_ok[s_sel]) {
            s_active = s_sel;
            ui_pixel_mascot_jump(s_mascot);
            lv_obj_delete(s_menu_scr);
            s_menu_scr = NULL;
            s_mascot = NULL;
            DEMOS[s_active].enter();
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            ui_pixel_mascot_jump(s_mascot);
        }
    }
    bsp_lvgl_unlock();
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport BSP demo 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    xTaskCreate(hb_task, "hb", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "M1 heartbeat up");

    bsp_i2c_init();
    ESP_LOGI(TAG, "M2 i2c bus up");

    // 屏幕是本 demo 的 UI 载体,失败就没有菜单可言 —— 打清楚日志后退出,
    // 不做"串口菜单"降级(那会让本文件复杂一倍,违背参考示例的初衷)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,demo 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);
    ESP_LOGI(TAG, "M3 display+lvgl up");

    s_ok[0] = true;                                   // Display 已确认可用
    s_ok[1] = (bsp_button_init(on_key, NULL) == ESP_OK);
    s_ok[4] = true;                                    // 页面内按需初始化并显示错误
    s_ok[5] = true;
    s_ok[6] = true;
    s_ok[7] = true;                                    // WoodFish: 按键+音频+存储 页面内自检
    s_ok[8] = true;                                    // Answer: 按键+音频 页面内自检, 无存储
    s_ok[9] = true;                                    // Places: 按键+Wi-Fi+存储 页面内自检
    ESP_LOGI(TAG, "M4 button ok=%d", s_ok[1]);

    // UI 必须先于 I2C 外设就绪: I2C 总线可能被总线上掉电不复位的从机(ES8311/CW2017)
    // 拖在异常状态, 扫描/事务可能长时间阻塞 —— 绝不能让菜单为音频/电量计陪葬。
    if (bsp_lvgl_lock(1000)) { enter_menu(); bsp_lvgl_unlock(); }
    ESP_LOGI(TAG, "M5 menu up");

    // 其余外设单项失败不阻塞:菜单里标 [FAIL],其他项照常可测。
    bsp_i2c_scan();
    ESP_LOGI(TAG, "M6 i2c scan done");
    s_ok[2] = (bsp_audio_init() == ESP_OK);
    ESP_LOGI(TAG, "M7 audio ok=%d", s_ok[2]);
    s_ok[3] = (bsp_battery_init() == ESP_OK);
    ESP_LOGI(TAG, "M8 battery ok=%d", s_ok[3]);
    if (bsp_lvgl_lock(500)) { menu_refresh(); bsp_lvgl_unlock(); }

    ESP_LOGI(TAG, "就绪:Display=%d Button=%d Audio=%d Battery=%d",
             s_ok[0], s_ok[1], s_ok[2], s_ok[3]);
}
