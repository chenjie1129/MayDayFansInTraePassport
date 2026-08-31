// main/demo_woodfish.c —— 敲木鱼玩法 (v2)。
//
// 按键语义:
//   OK 短按   -> 敲一下 (+1~+3 功德, 连击越高暴击 +10 概率越高)
//   OK 长按   -> 返回菜单 (由 main.c 统一拦截)
//   UP 短按   -> 切换皮肤 (经典木色 / 法喜金 / 电子紫)
//   DOWN 短按 -> 切换自动敲击节奏 (关 / 慢 / 中 / 快)
//
// 任务模型 (v2 关键整改):
//   * LVGL 任务 (taskLVGL) 里【绝不碰外设】: 不写 flash、不读 I2C、不阻塞写 I2S。
//     lv_timer 回调与按键回调只做内存模型更新 + queue 发送 + UI 属性设置。
//   * 所有慢操作 (bsp_audio_write 阻塞播放 / nvs_commit 写 flash / bsp_battery_soc
//     读 I2C) 全部在独立 wf_audio 任务中完成; 该任务低优先级 (3), 栈 4096,
//     随 app 常驻、幂等创建, enter/exit 不销毁。
//   * 视觉反馈不用 transform_scale: LVGL9 对 zoom 对象每帧申请 draw layer,
//     C3 无 PSRAM 会反复分配大块内存导致堆碎片/渲染饿死 IDLE。改用 body 底色
//     高亮闪烁 (每次敲击仅 2 次 set_style, 无 layout/transform 副作用)。
//   * NVS 磨损: 每 20 击或每 5 秒批量落盘, 退出时再补一次。
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"       // bsp_lvgl_lock/unlock
#include "bsp_battery.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "woodfish";

/* ---------------- 纯逻辑模型 ---------------- */
#define AUTO_MODES 4
static const uint32_t AUTO_INTERVAL_MS[AUTO_MODES] = { 0, 2000, 800, 350 };
static const char    *AUTO_LABELS[AUTO_MODES]      = { "OFF", "SLOW", "MID ", "FAST" };
#define SKINS 3
static const char    *SKIN_LABELS[SKINS]           = { "WOOD", "GOLD", "CYBER" };

/* 皮肤配色 (RGB888, 统一经 lv_color_hex() 转 RGB565) */
static const uint32_t BODY_RGB[SKINS]   = { 0xC9894A, 0xE8C260, 0x9A6CFF };
static const uint32_t DARK_RGB[SKINS]   = { 0x8A5A28, 0xA88324, 0x5B39B5 };
static const uint32_t LIGHT_RGB[SKINS]  = { 0xE2A569, 0xF7DE8E, 0xB895FF };
static const uint32_t STROKE_RGB[SKINS] = { 0x4A2F18, 0x5C4410, 0x2A1A5C };

typedef struct {
    uint32_t total_merit;       /* 累计 (NVS) */
    uint32_t session;           /* 本次会话 */
    uint32_t combo;             /* 连击 */
    uint32_t last_hit_ms;       /* 上次敲击时间 */
    uint32_t dirty_hits;        /* 未落盘的敲击数 */
    uint8_t  skin;              /* 0..SKINS-1 */
    uint8_t  auto_mode;         /* 0..AUTO_MODES-1 */
    uint8_t  flash;             /* body 高亮剩余 tick 数 */
    bool     sound_on;
} woodfish_t;

static woodfish_t s_m;

static uint8_t roll_merit(uint32_t combo) {
    uint32_t crit = 3 + (combo < 120 ? combo / 10 : 12);
    uint32_t r = esp_random() % 100;
    if (r < crit) return 10;          /* 暴击 */
    if (r < 40)   return 2;
    if (r < 75)   return 1;
    return 3;
}

/* ---------------- 持久化 (flash 写只允许 wf_audio 任务执行) ---------------- */
static nvs_handle_t s_nvs;
static bool         s_nvs_ok;

static void merit_open(void) {
    if (s_nvs_ok) return;            /* 常驻 handle, 幂等 */
    nvs_flash_init();
    s_nvs_ok = (nvs_open("woodfish", NVS_READWRITE, &s_nvs) == ESP_OK);
}
static void merit_load(uint32_t *out) {
    *out = 0;
    if (s_nvs_ok) nvs_get_u32(s_nvs, "total", out);
}
/* worker 任务上下文: 有脏数据才写, 写完清脏标 */
static void merit_flush(void) {
    if (!s_nvs_ok || s_m.dirty_hits == 0) return;
    nvs_set_u32(s_nvs, "total", s_m.total_merit);
    nvs_commit(s_nvs);
    s_m.dirty_hits = 0;
}

/* ---------------- 音频 (合成敲击音, worker 任务内使用) ---------------- */
#define KNOCK_SAMPLES 800      /* 16000Hz * 0.05s = 800 */
static int16_t s_knock_pcm[KNOCK_SAMPLES];
static int16_t s_crit_pcm[KNOCK_SAMPLES];

static void build_pcm(int16_t *buf, int base_freq, int end_freq, float gain) {
    int phase = 0;
    for (int i = 0; i < KNOCK_SAMPLES; i++) {
        float t = (float)i / KNOCK_SAMPLES;
        int freq = base_freq + (int)((end_freq - base_freq) * t);
        int period = 16000 / freq;
        float env = (1.0f - t) * (1.0f - t) * gain;
        float tri;
        int p = phase % period;
        if (p < period / 2) tri = -1.0f + 4.0f * p / period;
        else                tri =  3.0f - 4.0f * p / period;
        phase++;
        float nse = (float)((int)(esp_random() & 0xFFFF) - 0x8000) / 32768.0f;
        float nse_env = (t < 0.12f) ? (1.0f - t / 0.12f) : 0.0f;
        float s = tri * env * 0.7f + nse * nse_env * 0.4f * gain;
        int v = (int)(s * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}

/* ---------------- UI 对象与 worker 任务 ---------------- */
/* queue 消息: 1 = 普通敲击, 2 = 暴击, 99 = 退出前最终落盘 */
#define MSG_FLUSH 99
static QueueHandle_t s_audio_q;
static TaskHandle_t  s_audio_task;

static lv_obj_t  *s_scr, *s_fish, *s_total, *s_combo, *s_skin, *s_auto, *s_soc, *s_msg;
static lv_timer_t *s_timer;

/* 调用方必须持有 LVGL 锁 */
static void set_soc_locked(void) {
    if (!s_soc) return;
    int v = bsp_battery_soc();
    if (v < 0) lv_label_set_text(s_soc, "");
    else       lv_label_set_text_fmt(s_soc, "%d%%", v);
}

static void woodfish_draw_body(lv_obj_t *parent, int skin_idx) {
    /* 用 LVGL 基本形状 + 渐变拼木鱼, 不引入图片资源 (省 SRAM)。 */
    lv_color_t body   = lv_color_hex(BODY_RGB[skin_idx]);
    lv_color_t dark   = lv_color_hex(DARK_RGB[skin_idx]);
    lv_color_t stroke = lv_color_hex(STROKE_RGB[skin_idx]);

    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, 190, 116);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, body, 0);
    lv_obj_set_style_bg_grad_color(o, dark, 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(o, stroke, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_center(o);

    lv_obj_t *eye = lv_obj_create(o);
    lv_obj_set_size(eye, 8, 8);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_align(eye, LV_ALIGN_CENTER, -60, -22);

    lv_obj_t *shine = lv_obj_create(o);
    lv_obj_set_size(shine, 3, 3);
    lv_obj_set_style_radius(shine, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shine, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(shine, 0, 0);
    lv_obj_set_style_pad_all(shine, 0, 0);
    lv_obj_align(shine, LV_ALIGN_CENTER, -62, -24);

    lv_obj_t *mouth = lv_obj_create(o);
    lv_obj_set_size(mouth, 60, 10);
    lv_obj_set_style_radius(mouth, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mouth, stroke, 0);
    lv_obj_set_style_border_width(mouth, 0, 0);
    lv_obj_set_style_pad_all(mouth, 0, 0);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 18, 12);

    lv_obj_t *hl = lv_obj_create(o);
    lv_obj_set_size(hl, 70, 4);
    lv_obj_set_style_radius(hl, 2, 0);
    lv_obj_set_style_bg_color(hl, lv_color_hex(LIGHT_RGB[skin_idx]), 0);
    lv_obj_set_style_border_width(hl, 0, 0);
    lv_obj_set_style_pad_all(hl, 0, 0);
    lv_obj_align(hl, LV_ALIGN_CENTER, -20, -34);

    s_fish = o;
}

static void build_ui(void) {
    s_scr = ui_pixel_screen_create("WOODFISH");

    s_total = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_total, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_total, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_total, LV_ALIGN_TOP_MID, 0, 8);

    s_soc = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_soc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_soc, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_soc, LV_ALIGN_TOP_RIGHT, -8, 8);
    set_soc_locked();

    s_combo = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_combo, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_combo, lv_color_hex(UI_ORANGE), 0);
    lv_obj_align(s_combo, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_combo, "");

    woodfish_draw_body(s_scr, s_m.skin);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 230, 216, 72, UI_PAPER);
    s_skin = lv_label_create(panel);
    lv_obj_set_style_text_font(s_skin, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_skin, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_skin, LV_ALIGN_TOP_LEFT, 10, 6);

    s_auto = lv_label_create(panel);
    lv_obj_set_style_text_font(s_auto, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_auto, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_auto, LV_ALIGN_TOP_LEFT, 10, 26);

    s_msg = lv_label_create(panel);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_msg, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_label_set_text(s_msg, "OK knock");

    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA7AD), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
    lv_label_set_text(hint, "LONG OK -> MENU");

    ui_pixel_mascot_create(s_scr, 8, 248);
}

/* 调用方持 LVGL 锁; label 内部对相同文本会去重, 不会产生无效重绘 */
static void refresh_labels(void) {
    if (!s_scr) return;
    if (s_total) lv_label_set_text_fmt(s_total, "Merit %" PRIu32, s_m.total_merit);
    if (s_skin)  lv_label_set_text_fmt(s_skin,  "UP skin : %s", SKIN_LABELS[s_m.skin]);
    if (s_auto)  lv_label_set_text_fmt(s_auto,  "DW auto : %s", AUTO_LABELS[s_m.auto_mode]);
    if (s_combo) {
        if (s_m.combo > 1) lv_label_set_text_fmt(s_combo, "COMBO x%" PRIu32, s_m.combo);
        else               lv_label_set_text(s_combo, "");
    }
}

/* 敲击 (调用方持 LVGL 锁; 只做内存操作 + queue 发送, 绝不阻塞) */
static void hit_once_internal(bool critical) {
    uint8_t gain = roll_merit(s_m.combo);
    bool crit = (gain == 10) || critical;
    s_m.total_merit += gain;
    s_m.session     += gain;
    s_m.combo       += 1;
    s_m.last_hit_ms  = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_m.dirty_hits  += 1;
    /* 视觉反馈: body 高亮 2 个 tick (200ms), tick 里仅恢复时 set 一次样式 */
    s_m.flash = 2;
    if (s_fish) lv_obj_set_style_bg_color(s_fish, lv_color_hex(LIGHT_RGB[s_m.skin]), 0);
    int msg = crit ? 2 : 1;
    if (s_audio_q) xQueueSend(s_audio_q, &msg, 0);
    if (s_msg) lv_label_set_text_fmt(s_msg, crit ? "CRIT +%d" : "+%d", gain);
    refresh_labels();
}

/* worker 任务: 所有阻塞型外设操作的唯一归属地, 常驻、低优先级 */
static void audio_worker(void *arg) {
    (void)arg;
    bsp_audio_init();                 /* 幂等 (app_main 已 init 过) */
    build_pcm(s_knock_pcm, 260, 90,  0.75f);
    build_pcm(s_crit_pcm,  420, 120, 0.90f);
    bsp_audio_set_format(16000, 16, 1);
    bsp_audio_set_volume(80);

    uint32_t last_flush_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t last_soc_ms   = last_flush_ms;
    int msg = 0;
    while (1) {
        if (xQueueReceive(s_audio_q, &msg, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (msg == MSG_FLUSH) {        /* exit 请求最终落盘 */
                merit_flush();
                continue;
            }
            if (msg > 0 && s_m.sound_on) {
                int16_t *src = (msg == 2) ? s_crit_pcm : s_knock_pcm;
                bsp_audio_write(src, sizeof(s_knock_pcm));
            }
        }
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        /* 批量落盘: 20 击或 5 秒 */
        if (s_m.dirty_hits >= 20 || (s_m.dirty_hits > 0 && now - last_flush_ms >= 5000)) {
            merit_flush();
            last_flush_ms = now;
        }
        /* 电量刷新 ~30s: I2C 读在本任务, 短持 LVGL 锁更新文本, 拿不到就跳过 */
        if (now - last_soc_ms >= 30000) {
            last_soc_ms = now;
            if (bsp_lvgl_lock(300)) { set_soc_locked(); bsp_lvgl_unlock(); }
        }
    }
}

/* 每 100ms 一次, 全部是内存/UI 轻操作 (运行在 taskLVGL) */
static void tick(lv_timer_t *t) {
    (void)t;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* 高亮恢复: 只在归零的那一拍 set 一次样式 */
    if (s_m.flash > 0) {
        if (--s_m.flash == 0 && s_fish)
            lv_obj_set_style_bg_color(s_fish, lv_color_hex(BODY_RGB[s_m.skin]), 0);
    }

    /* 连击 1.5s 超时 */
    if (s_m.combo > 0 && now - s_m.last_hit_ms > 1500) {
        s_m.combo = 0;
        if (s_msg) lv_label_set_text(s_msg, "OK knock");
        refresh_labels();
    }

    /* 自动敲击 */
    static uint32_t last_auto = 0;
    uint32_t iv = AUTO_INTERVAL_MS[s_m.auto_mode];
    if (iv && now - last_auto >= iv) {
        hit_once_internal(false);
        last_auto = now;
    }
}

/* ---------------- 接口: enter / exit / key ---------------- */
void demo_woodfish_enter(void) {
    memset(&s_m, 0, sizeof(s_m));
    s_m.sound_on = true;
    merit_open();
    merit_load(&s_m.total_merit);

    /* worker 常驻: 仅首次创建, enter/exit 不销毁, 避免队列/任务生命周期竞态 */
    if (!s_audio_task) {
        s_audio_q = xQueueCreate(8, sizeof(int));
        xTaskCreate(audio_worker, "wf_audio", 4096, NULL, 3, &s_audio_task);
    }

    build_ui();
    refresh_labels();

    s_timer = lv_timer_create(tick, 100, NULL);
    lv_screen_load(s_scr);
    ESP_LOGI(TAG, "enter. total=%" PRIu32, s_m.total_merit);
}

void demo_woodfish_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    /* 请求 worker 做最终落盘 (队列里残留敲击播完后即执行, 百毫秒级) */
    if (s_audio_q) { int m = MSG_FLUSH; xQueueSend(s_audio_q, &m, 0); }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_fish = s_total = s_combo = s_skin = s_auto = s_soc = s_msg = NULL;
    }
    ESP_LOGI(TAG, "exit. session=%" PRIu32, s_m.session);
}

/* 按键回调由 main.c 持 LVGL 锁后调用 (OK LONG 已在 main.c 拦截为返回) */
void demo_woodfish_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        hit_once_internal(false);
    } else if (btn == BSP_BTN_UP) {
        s_m.skin = (s_m.skin + 1) % SKINS;
        if (s_fish) { lv_obj_delete(s_fish); s_fish = NULL; }
        s_m.flash = 0;
        woodfish_draw_body(s_scr, s_m.skin);
        refresh_labels();
    } else if (btn == BSP_BTN_DOWN) {
        s_m.auto_mode = (s_m.auto_mode + 1) % AUTO_MODES;
        refresh_labels();
    }
}
