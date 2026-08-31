// main/demo_place.c —— 离线「场所记忆」V0.2。
//
// 机制:每 60 秒做一次 Wi-Fi 全信道扫描,取信号最强的 12 个 BSSID 作为当前
// 地点指纹;与 NVS 中已保存的地点指纹做 Overlap 系数比对(交集/min(|A|,|B|)),
// >=50% 判定为同一地点(到访次数 +1)。不联网、不用 GPS。
//
// V0.2 抗误判(真机实测:设备不动,边界 AP 的 RSSI 抖动会让单次 Jaccard
// 跌破阈值而误建新地点):
//   * 指纹 8 -> 12 个 BSSID,弱信号边界的进出对整体相似度影响更小;
//   * Overlap 替代 Jaccard:本次多扫到几个弱 AP 不再拉低得分;
//   * 迟滞:得分 350‰~499‰ 为灰区,或得分 <350‰ 的第一次,都【不建点】,
//     只显示 CHECKING 并在 4 秒后快速重扫;连续 2 次明确不匹配才建新地点。
//
// 任务模型(遵循 AGENTS.md 不变量):
//   * LVGL 任务 / 按键回调【绝不阻塞】:只做内存状态更新、queue 发送、UI 属性设置。
//   * place worker(常驻、幂等创建,低优先级):Wi-Fi 扫描(阻塞 2~4s,协议栈
//     每次扫描后 stop+deinit)、指纹匹配、NVS 读写全部在此任务;更新 UI 时短持
//     bsp_lvgl_lock。
//   * NVS 磨损:新地点立即落盘(不能丢失发现);到访次数脏数据每 5 分钟节流
//     落盘一次,页面退出时补一次。
//
// 按键语义:
//   OK 短按   -> 立即重新扫描
//   UP/DOWN   -> 浏览已保存地点(循环)
//   OK 长按   -> 返回菜单(由 main.c 统一拦截)
#include "demo.h"
#include "bsp_display.h"        // bsp_lvgl_lock/unlock
#include "bsp_battery.h"
#include "bsp_wifi_scan.h"
#include "place_fp.h"
#include "ui_pixel.h"

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "place";

#define SCAN_INTERVAL_MS    60000    // 常规扫描周期
#define QUICK_RESCAN_MS     4000     // CHECKING(灰区/待确认)后的快速重扫间隔
#define FLUSH_THROTTLE_MS   300000   // 到访次数落盘节流:5 分钟
#define NVS_NAMESPACE       "placemem"
#define NVS_KEY_DB          "db"
#define PLACE_DB_MAGIC      0x504C4331u
#define PLACE_DB_VERSION    2        // V0.2: 指纹 8->12 BSSID,blob 布局变更

// worker 队列消息
#define MSG_ACTIVE  1   // 进入页面:开始周期扫描(立即扫一次)
#define MSG_IDLE    2   // 退出页面:停止扫描并落盘
#define MSG_SCAN    3   // OK 短按:立即重扫

/* ---------------- 持久化(仅 worker 任务访问,NVS blob ≈1KB) ---------------- */
static place_db_t   s_db;
static nvs_handle_t s_nvs;
static bool         s_nvs_ok;
static bool         s_dirty;          // 有未落盘的到访次数
static SemaphoreHandle_t s_db_mutex; // 保护 s_db:worker 写 / 按键回调读(浏览)

static void db_reset(void)
{
    memset(&s_db, 0, sizeof(s_db));
    s_db.magic   = PLACE_DB_MAGIC;
    s_db.version = PLACE_DB_VERSION;
    s_db.count   = 0;
}

// 仅在 worker 启动时调用一次。
static void db_load(void)
{
    nvs_flash_init();   // 幂等;失败不擦除分区
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs) != ESP_OK) {
        s_nvs_ok = false;
        ESP_LOGE(TAG, "NVS 打开失败,地点无法持久化(本次运行仍可扫描)");
        db_reset();
        return;
    }
    s_nvs_ok = true;
    place_db_t tmp;
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(s_nvs, NVS_KEY_DB, &tmp, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        db_reset();   // 首次使用,库本来就为空
    } else if (err != ESP_OK || len != sizeof(tmp) ||
               tmp.magic != PLACE_DB_MAGIC || tmp.version != PLACE_DB_VERSION ||
               tmp.count > PLACE_MAX_COUNT) {
        // 固件升级导致 blob 布局变化(如 V0.1 的 8-BSSID 指纹)时,旧库作废重建,
        // 绝不按新布局解析旧字节(会把相邻记录当 BSSID 读入垃圾)。
        ESP_LOGW(TAG, "地点库不兼容(err=%s len=%u ver=%u),已重置为空库",
                 esp_err_to_name(err), (unsigned)len,
                 (err == ESP_OK) ? tmp.version : 0u);
        db_reset();
    } else {
        s_db = tmp;
    }
    ESP_LOGI(TAG, "已加载 %u 个已保存地点", (unsigned)s_db.count);
}

// 调用方必须持有 s_db_mutex。
static void db_flush_locked(void)
{
    if (!s_nvs_ok || !s_dirty) return;
    nvs_set_blob(s_nvs, NVS_KEY_DB, &s_db, sizeof(s_db));
    nvs_commit(s_nvs);
    s_dirty = false;
}

/* ---------------- UI 对象 ---------------- */
static lv_obj_t  *s_scr;
static lv_obj_t  *s_lbl_status, *s_lbl_fp, *s_lbl_match, *s_lbl_saved;
static lv_obj_t  *s_lbl_browse, *s_lbl_next, *s_lbl_soc;
static lv_timer_t *s_timer;

static QueueHandle_t s_q;
static TaskHandle_t  s_worker;
static volatile bool s_active;       // 页面是否在前台
static volatile bool s_scanning;     // 当前是否阻塞扫描中
static volatile uint32_t s_next_due; // 下次计划扫描的 tick(ms)
static int s_browse = -1;            // 浏览中的地点索引;-1 = 实时视图
static place_session_t s_sess;       // 迟滞判定会话(进页面清零,仅 worker 访问)

static const char *strength_label(int avg_rssi)
{
    if (avg_rssi >= -50) return "STRONG";
    if (avg_rssi >= -65) return "GOOD";
    if (avg_rssi >= -75) return "WEAK";
    return "POOR";
}

// 所有 worker -> UI 的更新都走这里:短持 LVGL 锁,页面退出后(s_scr==NULL)直接跳过。
static void ui_publish(const char *status, uint32_t status_color,
                       const char *fp, const char *match, const char *saved)
{
    if (!bsp_lvgl_lock(300)) return;
    if (s_scr) {
        if (s_lbl_status) {
            lv_label_set_text(s_lbl_status, status);
            lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(status_color), 0);
        }
        if (fp && s_lbl_fp)        lv_label_set_text(s_lbl_fp, fp);
        if (match && s_lbl_match)  lv_label_set_text(s_lbl_match, match);
        if (saved && s_lbl_saved)  lv_label_set_text(s_lbl_saved, saved);
        s_browse = -1;
        if (s_lbl_browse) lv_label_set_text(s_lbl_browse, "LIVE");
        if (s_lbl_soc) {
            int soc = bsp_battery_soc();
            if (soc < 0) lv_label_set_text(s_lbl_soc, "");
            else         lv_label_set_text_fmt(s_lbl_soc, "%d%%", soc);
        }
    }
    bsp_lvgl_unlock();
}

/* ---------------- 扫描周期(worker 任务) ---------------- */
// 返回距下次扫描的等待毫秒:CHECKING(灰区/待确认)时 4s 快速重扫,其余 60s。
static uint32_t run_cycle(void)
{
    s_scanning = true;
    ui_publish("SCANNING...", UI_SKY_DARK, NULL, NULL, NULL);

    bsp_wifi_ap_t aps[BSP_WIFI_SCAN_MAX];
    size_t n = 0;
    esp_err_t err = bsp_wifi_scan_once(aps, BSP_WIFI_SCAN_MAX, &n);

    s_scanning = false;
    if (!s_active) return SCAN_INTERVAL_MS;   // 扫描期间页面已退出,结果丢弃

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 扫描失败: %s", esp_err_to_name(err));
        char mline[48];
        snprintf(mline, sizeof(mline), "WI-FI FAIL: %s", esp_err_to_name(err));
        ui_publish("WI-FI FAIL", UI_RED, "FP: --", mline, NULL);
        return SCAN_INTERVAL_MS;
    }

    place_ap_t pa[BSP_WIFI_SCAN_MAX];
    for (size_t i = 0; i < n; i++) {
        memcpy(pa[i].bssid, aps[i].bssid, 6);
        pa[i].rssi = aps[i].rssi;
    }

    place_fingerprint_t fp;
    place_fp_build(&fp, pa, n);
    if (fp.count == 0) {
        ui_publish("NO AP FOUND", UI_RED, "FP: 0 APs", "move around and rescan", NULL);
        return SCAN_INTERVAL_MS;
    }

    // 指纹强度:指纹内 12 个 BSSID 的平均 RSSI
    int sum = 0, cnt = 0;
    for (uint8_t i = 0; i < fp.count; i++) {
        for (size_t j = 0; j < n; j++) {
            if (memcmp(fp.bssid[i], pa[j].bssid, 6) == 0) { sum += pa[j].rssi; cnt++; break; }
        }
    }
    int avg = cnt ? sum / cnt : 0;

    int  idx = -1, score = -1;
    bool is_new = false, checking = false, unsure = false;
    const char *dec = "?";
    unsigned saved_count = 0;
    if (xSemaphoreTake(s_db_mutex, pdMS_TO_TICKS(2000))) {
        idx = place_db_find(&s_db, &fp, &score);
        place_obs_t obs;
        if (idx >= 0) {
            s_sess.new_streak = 0;   // 命中,迟滞连击清零(observe 内部同理,双保险)
            obs = PLACE_OBS_MATCH;
        } else if (s_db.count == 0) {
            obs = PLACE_OBS_NEW_COMMIT;   // 空库:首个地点直接建立,无需二次确认
        } else {
            obs = place_fp_observe(&s_sess, score);
        }

        if (obs == PLACE_OBS_MATCH) {
            s_db.places[idx].visits++;
            s_dirty = true;
            dec = "KNOWN";
        } else if (obs == PLACE_OBS_NEW_COMMIT && s_db.count < PLACE_MAX_COUNT) {
            idx = s_db.count;
            place_record_t *r = &s_db.places[idx];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "PLACE %02u", (unsigned)(idx + 1));
            r->visits = 1;
            r->fp = fp;
            s_db.count++;
            s_dirty = true;
            db_flush_locked();   // 新地点立即落盘,断电不丢
            is_new = true;
            dec = "NEW";
        } else if (s_db.count >= PLACE_MAX_COUNT) {
            dec = "FULL";        // 库满:不进入快速重扫循环,按常规周期重试
        } else if (obs == PLACE_OBS_UNSURE) {
            checking = true;     // 灰区:不建点,4s 后快速重扫
            unsure = true;
            dec = "UNSURE";
        } else {
            checking = true;     // NEW_PENDING:第一次明确不像,4s 后再确认一次
            dec = "PENDING";
        }
        saved_count = s_db.count;
        xSemaphoreGive(s_db_mutex);

        char fp_line[48], match_line[48], saved_line[32], status_line[32];
        snprintf(fp_line, sizeof(fp_line), "FP: %u APs  %d dBm  %s",
                 fp.count, avg, strength_label(avg));
        if (is_new) {
            snprintf(status_line, sizeof(status_line), "NEW PLACE #%d!", idx + 1);
            snprintf(match_line, sizeof(match_line), "SAVED AS #%d", idx + 1);
        } else if (idx >= 0) {
            snprintf(status_line, sizeof(status_line), "KNOWN PLACE #%d", idx + 1);
            snprintf(match_line, sizeof(match_line), "MATCH %d%%", score / 10);
        } else if (checking) {
            // 迟滞态:绝不显示"新地点",只提示正在复核,避免误导
            snprintf(status_line, sizeof(status_line), "CHECKING...");
            if (unsure && score >= 0)
                snprintf(match_line, sizeof(match_line), "WEAK %d%% - RESCAN SOON", score / 10);
            else
                snprintf(match_line, sizeof(match_line), "NEW? CONFIRMING...");
        } else {
            snprintf(status_line, sizeof(status_line), "UNKNOWN PLACE");
            snprintf(match_line, sizeof(match_line), "LIBRARY FULL %d/%d",
                     PLACE_MAX_COUNT, PLACE_MAX_COUNT);
        }
        snprintf(saved_line, sizeof(saved_line), "SAVED %u/%d", saved_count, PLACE_MAX_COUNT);

        ESP_LOGI(TAG, "APs=%u fp=%u avg=%ddBm -> %s (idx=%d score=%d permille, streak=%u, saved=%u)",
                 (unsigned)n, fp.count, avg, dec, idx, score,
                 (unsigned)s_sess.new_streak, saved_count);

        ui_publish(status_line,
                   is_new ? UI_ORANGE :
                   (idx >= 0 ? UI_GRASS_DARK :
                    (checking ? UI_ORANGE : UI_RED)),
                   fp_line, match_line, saved_line);
    } else {
        ESP_LOGE(TAG, "获取地点库互斥量超时");
    }
    return checking ? QUICK_RESCAN_MS : SCAN_INTERVAL_MS;
}

static void place_worker(void *arg)
{
    (void)arg;
    db_load();

    uint32_t last_flush = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t msg = 0;
    while (1) {
        if (xQueueReceive(s_q, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (msg == MSG_IDLE) {
                s_active = false;
                s_scanning = false;
                if (xSemaphoreTake(s_db_mutex, pdMS_TO_TICKS(2000))) {
                    db_flush_locked();   // 退出前补一次落盘
                    xSemaphoreGive(s_db_mutex);
                }
                continue;
            }
            if (msg == MSG_ACTIVE) {
                s_active = true;
                s_sess.new_streak = 0;   // 每次进页面重新开始迟滞判定
                s_next_due = now;        // 立即触发首次扫描
            } else if (msg == MSG_SCAN && s_active) {
                s_next_due = now;
            }
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (s_active && (int32_t)(now - s_next_due) >= 0) {
            // CHECKING(灰区/待确认)时 run_cycle 返回 4s 快速重扫,否则 60s。
            uint32_t next_delay = run_cycle();
            s_next_due = xTaskGetTickCount() * portTICK_PERIOD_MS + next_delay;
            now = s_next_due;
            last_flush = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }

        // 到访次数节流落盘(新地点已在 run_cycle 内立即落盘)
        if (s_active && s_dirty &&
            (int32_t)(now - last_flush) >= FLUSH_THROTTLE_MS &&
            xSemaphoreTake(s_db_mutex, pdMS_TO_TICKS(2000))) {
            db_flush_locked();
            xSemaphoreGive(s_db_mutex);
            last_flush = now;
        }
    }
}

/* ---------------- 页面构建 ---------------- */
static void build_ui(void)
{
    s_scr = ui_pixel_screen_create("PLACES");

    s_lbl_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_lbl_status, 220);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 50);
    lv_label_set_text(s_lbl_status, "STARTING...");

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 84, 220, 140, UI_PAPER);

    s_lbl_fp = lv_label_create(panel);
    lv_obj_set_style_text_font(s_lbl_fp, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_fp, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_lbl_fp, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_label_set_text(s_lbl_fp, "FP: -- APs");

    s_lbl_match = lv_label_create(panel);
    lv_obj_set_style_text_font(s_lbl_match, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_match, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_lbl_match, LV_ALIGN_TOP_LEFT, 2, 26);
    lv_label_set_text(s_lbl_match, "MATCH --");

    s_lbl_saved = lv_label_create(panel);
    lv_obj_set_style_text_font(s_lbl_saved, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_saved, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_lbl_saved, LV_ALIGN_TOP_LEFT, 2, 50);
    lv_label_set_text_fmt(s_lbl_saved, "SAVED 0/%d", PLACE_MAX_COUNT);

    s_lbl_browse = lv_label_create(panel);
    lv_obj_set_style_text_font(s_lbl_browse, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_browse, lv_color_hex(UI_GRASS_DARK), 0);
    lv_obj_align(s_lbl_browse, LV_ALIGN_TOP_LEFT, 2, 76);
    lv_label_set_text(s_lbl_browse, "LIVE");

    s_lbl_next = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_lbl_next, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_next, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_text_align(s_lbl_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_lbl_next, 220);
    lv_obj_align(s_lbl_next, LV_ALIGN_TOP_MID, 0, 252);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, 220);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 228);
    lv_label_set_text(hint, "OK: scan   UP/DN: view");

    // 电量放右上角云饰下方,避开标题牌(x<=156)与云朵(y<=25)
    s_lbl_soc = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_lbl_soc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_soc, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_lbl_soc, LV_ALIGN_TOP_RIGHT, -8, 27);

    // 吉祥物站草地(y272..320),与 hint(y228)/倒计时(y252)两行文字不重叠
    ui_pixel_mascot_create(s_scr, 101, 272);
}

// 200ms 一拍:仅刷新"下次扫描倒计时"(纯内存/UI 轻操作,运行在 LVGL 任务)。
static void tick(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_next) return;
    if (s_scanning) {
        lv_label_set_text(s_lbl_next, "");
        return;
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int32_t rem = (int32_t)(s_next_due - now);
    if (s_active && rem > 0) lv_label_set_text_fmt(s_lbl_next, "next scan: %ds", (int)(rem / 1000));
    else                     lv_label_set_text(s_lbl_next, "");
}

/* ---------------- enter / exit / key ---------------- */
void demo_place_enter(void)
{
    // worker 常驻:仅首次创建,enter/exit 不销毁,避免队列/任务生命周期竞态。
    if (!s_worker) {
        s_db_mutex = xSemaphoreCreateMutex();
        s_q = xQueueCreate(8, sizeof(uint32_t));
        xTaskCreate(place_worker, "place", 6144, NULL, 3, &s_worker);
    }

    build_ui();
    s_browse = -1;
    s_timer = lv_timer_create(tick, 200, NULL);
    lv_screen_load(s_scr);

    uint32_t msg = MSG_ACTIVE;
    xQueueSend(s_q, &msg, 0);
}

void demo_place_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    uint32_t msg = MSG_IDLE;
    if (s_q) xQueueSend(s_q, &msg, 0);
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_lbl_status = s_lbl_fp = s_lbl_match = s_lbl_saved = NULL;
        s_lbl_browse = s_lbl_next = s_lbl_soc = NULL;
    }
}

// 由 main.c 持 LVGL 锁后调用(OK 长按已被 main.c 拦截为返回)。
// 只发队列消息 / 读内存模型,绝不阻塞。
void demo_place_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        uint32_t msg = MSG_SCAN;
        if (s_q) xQueueSend(s_q, &msg, 0);
        return;
    }
    if (btn != BSP_BTN_UP && btn != BSP_BTN_DOWN) return;
    if (!s_lbl_browse) return;

    int dir = (btn == BSP_BTN_DOWN) ? 1 : -1;
    if (xSemaphoreTake(s_db_mutex, pdMS_TO_TICKS(200))) {
        unsigned count = s_db.count;
        if (count > 0) {
            if (s_browse < 0) s_browse = (dir > 0) ? 0 : (int)count - 1;
            else              s_browse = (s_browse + dir + (int)count) % (int)count;
            place_record_t *r = &s_db.places[s_browse];
            lv_label_set_text_fmt(s_lbl_browse,
                                  "VIEW #%d  %s\nvisits: %" PRIu16 "   APs: %d",
                                  s_browse + 1, r->name, r->visits, r->fp.count);
        }
        xSemaphoreGive(s_db_mutex);
    }
}
