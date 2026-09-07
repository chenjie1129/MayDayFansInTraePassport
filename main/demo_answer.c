// main/demo_answer.c —— 答案之书玩法。
//
// 心中默念一个问题, 按 OK: 后台连接 Wi-Fi 并调用 DeepSeek, 模型返回本地
// 答案库中的编号后揭晓。三副牌组 (经典 / 打工人 / 程序员) 可切换。
//
// 按键语义:
//   OK 短按   -> 封面/答案页: 开始翻书; 翻书中: 忽略 (防抖)
//   UP/DOWN   -> 切换牌组 (翻书中忽略)
//   OK 长按   -> 返回菜单 (由 main.c 统一拦截)
//
// 任务模型 (与 demo_woodfish.c 同一纪律):
//   * LVGL 任务里【绝不碰外设】: 按键回调与 lv_timer 只改内存状态/UI 属性/发队列。
//   * 揭晓音在 DeepSeek 请求完成后才创建临时 ans_audio 任务；播放后立即关闭 codec
//     并释放任务栈，避免无 PSRAM 设备上的 TLS 握手内存不足。
//   * 视觉反馈零 draw-layer 分配: 悬念动画只用 opa 闪烁/文本切换; 答案淡入用
//     style_opa 动画 (无 zoom/rotation, C3 无 PSRAM 下每帧分配 layer 会饿死 IDLE)。
//   * Wi-Fi 凭证和 DeepSeek API Key 只从本次开机的 RAM 会话读取。
//   * API 只返回 0..15 的答案编号，屏幕仍使用受控本地字库，拒绝任意文本。
//
// 中文渲染: 答案/书名/牌组名使用子集字体 lv_font_answer_24 (由 main/answers.json
// 经 tools/gen_answer_data.js + lv_font_conv 生成, 仅含 176 个用到的汉字)。
// 【坑】lv_font_conv 必须加 --no-compress: 它默认输出压缩位图 (bitmap_format=1),
// 而本固件未开 CONFIG_LV_USE_FONT_COMPRESSED, LVGL 解压分支直接返回 NULL ——
// 所有汉字静默不渲染, 页面只剩米白纸页, 表现为"白屏"。
#include "demo.h"
#include "bsp_audio.h"
#include "ui_pixel.h"
#include "answer_data.h"
#include "deepseek_answer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "answer";

LV_FONT_DECLARE(lv_font_answer_24);

/* ---------------- 纯逻辑模型 ---------------- */
enum {
    STATE_COVER = 0,
    STATE_REQUESTING = 1,
    STATE_ANSWER = 2,
    STATE_ERROR = 3,
};

#define SHAKE_MS      1300   /* 翻书悬念时长 */
#define SHAKE_FRAME   180    /* 帧切换间隔 */
#define AUDIO_RATE    16000
#define PING_LEN      1920   /* 0.12s 单音 */

static uint8_t s_state;
static uint8_t s_deck;
static int     s_last[ANSWER_DECK_COUNT];

static TaskHandle_t s_audio_task;

/* ---------------- UI 对象 ---------------- */
static lv_obj_t  *s_scr, *s_book, *s_page, *s_q, *s_dots;
static lv_obj_t  *s_answer, *s_deck_cn, *s_hint;
static lv_timer_t *s_timer;
static uint32_t  s_request_start;

/* ---------------- 音频合成 (worker 任务内使用) ---------------- */
static void clamp_pcm(int16_t *buf, int n) {
    for (int i = 0; i < n; i++) {
        int v = buf[i];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}

/* 揭晓音: 钟声 ping (基频 + 二次谐波, 指数衰减) */
static void build_ping(int16_t *buf, int freq) {
    float phase = 0.0f;
    float step = 2.0f * 3.14159265f * freq / AUDIO_RATE;
    for (int i = 0; i < PING_LEN; i++) {
        float t = (float)i / AUDIO_RATE;
        float env = expf(-t * 16.0f);
        float s = (sinf(phase) * 0.75f + sinf(phase * 2.0f) * 0.20f) * env * 0.55f;
        phase += step;
        buf[i] = (int16_t)(s * 32767.0f);
    }
    clamp_pcm(buf, PING_LEN);
}

static void answer_audio_worker(void *arg) {
    (void)arg;
    static int16_t pcm[PING_LEN];
    static const int ping_freq[3] = { 1046, 1318, 1568 };  /* C6 E6 G6 上行 */
    if (bsp_audio_init() == ESP_OK &&
        bsp_audio_set_format(AUDIO_RATE, 16, 1) == ESP_OK) {
        bsp_audio_set_volume(75);
        for (int i = 0; i < 3; i++) {
            build_ping(pcm, ping_freq[i]);
            bsp_audio_write(pcm, PING_LEN * (int)sizeof(int16_t));
            vTaskDelay(pdMS_TO_TICKS(45));
        }
    }
    bsp_audio_close();
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static void play_reveal_sound(void) {
    if (s_audio_task != NULL) return;
    if (xTaskCreate(answer_audio_worker, "ans_audio", 4096, NULL, 3,
                    &s_audio_task) != pdPASS) {
        s_audio_task = NULL;
        ESP_LOGW(TAG, "Unable to allocate reveal audio task");
    }
}

/* ---------------- UI 构建 ---------------- */
static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

/* 合着的书: 深紫封面 + 书脊 + 金线 + 书名 */
static void build_book(lv_obj_t *parent) {
    s_book = lv_obj_create(parent);
    lv_obj_remove_flag(s_book, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_book, 40, 100);
    lv_obj_set_size(s_book, 160, 140);
    lv_obj_set_style_radius(s_book, 6, 0);
    lv_obj_set_style_bg_color(s_book, lv_color_hex(0x3D2B5E), 0);
    lv_obj_set_style_border_color(s_book, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_book, 4, 0);
    lv_obj_set_style_pad_all(s_book, 0, 0);

    block(s_book, 10, 8, 6, 124, 0x2A1D44);             /* 书脊 */
    block(s_book, 28, 26, 104, 2, UI_YELLOW);           /* 金线 */
    block(s_book, 28, 112, 104, 2, UI_YELLOW);
    block(s_book, 76, 64, 8, 8, UI_YELLOW);             /* 中心装饰 */

    lv_obj_t *title = lv_label_create(s_book);
    lv_label_set_text(title, "答案之书");
    lv_obj_set_style_text_font(title, &lv_font_answer_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_YELLOW), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 6, -8);
}

/* 翻开的书页: 米白面板, 翻书时显示 "?" 动画, 揭晓后显示答案。
 * 注意: 不能用 ui_pixel_panel_create() —— 它把阴影画成 parent 的兄弟对象,
 * 本页 HIDDEN 时阴影不会跟着隐藏, 会残留在屏幕上盖住紫书封面。
 * 这里用透明容器把阴影+书页都收作子对象, hide 容器即整体消失。 */
static void build_page(lv_obj_t *parent) {
    s_page = lv_obj_create(parent);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_page, 20, 100);
    lv_obj_set_size(s_page, 200, 140);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_TRANSP, 0);

    block(s_page, 5, 6, 200, 140, UI_INK);                 /* 阴影 (子对象) */
    lv_obj_t *paper = block(s_page, 0, 0, 200, 140, UI_PAPER);
    lv_obj_set_style_border_color(paper, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(paper, 4, 0);
    lv_obj_set_style_pad_all(paper, 7, 0);

    s_q = lv_label_create(paper);
    lv_label_set_text(s_q, "?");
    lv_obj_set_style_text_font(s_q, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_q, lv_color_hex(0x8A7FB0), 0);
    lv_obj_align(s_q, LV_ALIGN_CENTER, 0, -26);

    s_dots = lv_label_create(paper);
    lv_label_set_text(s_dots, ".");
    lv_obj_set_style_text_font(s_dots, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_dots, lv_color_hex(0x8A7FB0), 0);
    lv_obj_align(s_dots, LV_ALIGN_CENTER, 0, 14);

    s_answer = lv_label_create(paper);
    lv_label_set_text(s_answer, "");
    lv_obj_set_style_text_font(s_answer, &lv_font_answer_24, 0);
    lv_obj_set_style_text_color(s_answer, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, 176);
    lv_obj_set_style_text_align(s_answer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_answer);
}

static void build_ui(void) {
    s_scr = ui_pixel_screen_create("ANSWER");

    lv_obj_t *deck_en = lv_label_create(s_scr);
    lv_label_set_text(deck_en, "DECK");
    lv_obj_set_style_text_font(deck_en, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(deck_en, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(deck_en, LV_ALIGN_TOP_MID, 0, 48);

    s_deck_cn = lv_label_create(s_scr);
    lv_label_set_text(s_deck_cn, ANSWER_DECK_NAMES[0]);
    lv_obj_set_style_text_font(s_deck_cn, &lv_font_answer_24, 0);
    lv_obj_set_style_text_color(s_deck_cn, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_deck_cn, LV_ALIGN_TOP_MID, 0, 64);

    build_book(s_scr);
    build_page(s_scr);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5A6A72), 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 12, 254);

    ui_pixel_mascot_create(s_scr, 196, 248);
}

/* ---------------- 状态切换 (调用方持 LVGL 锁) ---------------- */
static void show_cover(void) {
    s_state = STATE_COVER;
    lv_obj_remove_flag(s_book, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    deepseek_answer_snapshot_t network;
    deepseek_answer_get_snapshot(&network);
    if (network.state == DEEPSEEK_ANSWER_NEEDS_WIFI) {
        lv_label_set_text(s_hint, "OPEN WI-FI APP FIRST");
    } else if (network.state == DEEPSEEK_ANSWER_NEEDS_API_KEY) {
        lv_label_set_text(s_hint, "ADD API KEY IN WI-FI APP");
    } else if (network.state == DEEPSEEK_ANSWER_CONNECTING) {
        lv_label_set_text(s_hint, "CONNECTING...");
    } else if (network.state == DEEPSEEK_ANSWER_ERROR) {
        lv_label_set_text(s_hint, "NETWORK UNAVAILABLE");
    } else {
        lv_label_set_text(s_hint, "OK: ASK    UP/DOWN: DECK");
    }
}

static void start_request(void) {
    if (deepseek_answer_request(s_deck) != ESP_OK) {
        show_cover();
        return;
    }
    s_state = STATE_REQUESTING;
    s_request_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    lv_obj_add_flag(s_book, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_q, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_answer, "");
    lv_obj_set_style_opa(s_q, LV_OPA_COVER, 0);
    lv_label_set_text(s_hint, "READING...");
}

/* 答案淡入: style_opa 动画, 不分配 draw layer */
static void set_obj_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void reveal(int idx) {
    s_state = STATE_ANSWER;
    if (idx < 0 || idx >= ANSWERS_PER_DECK) idx = 0;
    if (idx == s_last[s_deck]) idx = (idx + 1) % ANSWERS_PER_DECK;
    s_last[s_deck] = idx;

    lv_label_set_text(s_answer, ANSWERS[s_deck][idx]);
    lv_obj_add_flag(s_q, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_answer);
    lv_anim_set_exec_cb(&a, set_obj_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_label_set_text(s_hint, "OK: AGAIN    LONG: MENU");
    play_reveal_sound();
    ESP_LOGI(TAG, "reveal deck=%d idx=%d: %s", s_deck, idx, ANSWERS[s_deck][idx]);
}

static void show_request_error(deepseek_answer_error_t error, int http_status) {
    s_state = STATE_ERROR;
    lv_obj_add_flag(s_q, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_answer, LV_OPA_COVER, 0);
    switch (error) {
    case DEEPSEEK_ERROR_AUTH:
        lv_label_set_text(s_answer, "API KEY REJECTED");
        lv_label_set_text(s_hint, "UPDATE KEY IN WI-FI APP");
        break;
    case DEEPSEEK_ERROR_RATE_LIMIT:
        lv_label_set_text(s_answer, "RATE LIMITED");
        lv_label_set_text(s_hint, "WAIT, THEN OK TO RETRY");
        break;
    case DEEPSEEK_ERROR_TIMEOUT:
        lv_label_set_text(s_answer, "REQUEST TIMEOUT");
        lv_label_set_text(s_hint, "OK: RETRY");
        break;
    case DEEPSEEK_ERROR_WIFI:
        lv_label_set_text(s_answer, "NETWORK ERROR");
        lv_label_set_text(s_hint, "CONNECT WI-FI FIRST");
        break;
    case DEEPSEEK_ERROR_RESPONSE:
        lv_label_set_text(s_answer, "BAD API RESPONSE");
        lv_label_set_text(s_hint, "OK: RETRY");
        break;
    default:
        lv_label_set_text_fmt(s_answer, "API ERROR\nHTTP %d", http_status);
        lv_label_set_text(s_hint, "OK: RETRY");
        break;
    }
}

/* 每 100ms: 轮询 worker 快照并驱动零分配的等待动画。 */
static void tick(lv_timer_t *t) {
    (void)t;
    deepseek_answer_snapshot_t network;
    deepseek_answer_get_snapshot(&network);

    if (s_state == STATE_COVER) {
        show_cover();
        return;
    }
    if (s_state != STATE_REQUESTING) return;

    uint32_t elapsed =
        (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_request_start;
    uint32_t frame = elapsed / SHAKE_FRAME;
    lv_obj_set_style_opa(s_q, (frame & 1U) ? LV_OPA_30 : LV_OPA_COVER, 0);
    static const char *dots[3] = { ".", "..", "..." };
    lv_label_set_text(s_dots, dots[frame % 3]);
    if (network.state == DEEPSEEK_ANSWER_RESULT &&
        elapsed >= SHAKE_MS) {
        reveal(network.answer_index);
    } else if (network.state == DEEPSEEK_ANSWER_ERROR) {
        show_request_error(network.error, network.http_status);
    }
}

/* ---------------- 接口: enter / exit / key ---------------- */
void demo_answer_enter(void) {
    s_state = STATE_COVER;
    s_deck = 0;
    for (int i = 0; i < ANSWER_DECK_COUNT; i++) s_last[i] = -1;

    deepseek_answer_start();
    build_ui();
    show_cover();

    s_timer = lv_timer_create(tick, 100, NULL);
    lv_screen_load(s_scr);
    ESP_LOGI(TAG, "enter: %d decks x %d answers", ANSWER_DECK_COUNT, ANSWERS_PER_DECK);
}

void demo_answer_exit(void) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    deepseek_answer_stop();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_book = s_page = s_q = s_dots = s_answer = s_deck_cn = s_hint = NULL;
    }
    ESP_LOGI(TAG, "exit");
}

/* 按键回调由 main.c 持 LVGL 锁后调用 (OK LONG 已在 main.c 拦截为返回) */
void demo_answer_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        if (s_state == STATE_COVER ||
            s_state == STATE_ANSWER ||
            s_state == STATE_ERROR) {
            start_request();
        }
    } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (s_state == STATE_REQUESTING) return;
        int dir = (btn == BSP_BTN_UP) ? (ANSWER_DECK_COUNT - 1) : 1;
        s_deck = (uint8_t)((s_deck + dir) % ANSWER_DECK_COUNT);
        lv_label_set_text(s_deck_cn, ANSWER_DECK_NAMES[s_deck]);
    }
}
