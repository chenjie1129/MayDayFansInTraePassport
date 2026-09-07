// Wi-Fi connection UI. The worker owns AP/STA, HTTP, DNS and credentials.
#include "demo.h"
#include "ui_pixel.h"
#include "wifi_portal.h"

#include "esp_err.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_footer;
static lv_timer_t *s_timer;
static wifi_portal_snapshot_t s_rendered;
static bool s_have_rendered;

static const char *state_title(wifi_portal_state_t state)
{
    switch (state) {
    case WIFI_PORTAL_STARTING: return "STARTING";
    case WIFI_PORTAL_WAITING: return "PHONE SETUP";
    case WIFI_PORTAL_SCANNING: return "SCANNING";
    case WIFI_PORTAL_CONNECTING: return "CONNECTING";
    case WIFI_PORTAL_CHECKING: return "CHECKING INTERNET";
    case WIFI_PORTAL_ONLINE: return "ONLINE";
    case WIFI_PORTAL_RECONNECTING: return "RECONNECTING";
    case WIFI_PORTAL_FAILED: return "CONNECTION FAILED";
    case WIFI_PORTAL_STOPPED:
    default: return "STOPPED";
    }
}

static void render_snapshot(const wifi_portal_snapshot_t *snapshot)
{
    lv_label_set_text(s_status, state_title(snapshot->state));
    switch (snapshot->state) {
    case WIFI_PORTAL_STARTING:
        lv_label_set_text(s_detail, "Preparing secure setup hotspot...");
        break;
    case WIFI_PORTAL_WAITING:
        lv_label_set_text_fmt(
            s_detail,
            "%s\nPASS  %s\nOPEN  192.168.4.1\n%u networks found",
            snapshot->setup_ssid[0] ? snapshot->setup_ssid : "WIFI_SETUP_......",
            snapshot->setup_password[0] ? snapshot->setup_password : "............",
            (unsigned)snapshot->access_point_count);
        break;
    case WIFI_PORTAL_SCANNING:
        lv_label_set_text(s_detail,
                          "Phone requested a 2.4 GHz scan.\nPlease wait...");
        break;
    case WIFI_PORTAL_CONNECTING:
        lv_label_set_text_fmt(s_detail, "%s\nWaiting for an IP address...",
                              snapshot->target_ssid);
        break;
    case WIFI_PORTAL_CHECKING:
        lv_label_set_text_fmt(s_detail, "%s\nIP  %s\nTesting DNS + HTTPS...",
                              snapshot->target_ssid, snapshot->ip);
        break;
    case WIFI_PORTAL_ONLINE:
        lv_label_set_text_fmt(
            s_detail, "%s\nIP  %s\nRSSI  %d dBm\nInternet  %s",
            snapshot->target_ssid, snapshot->ip, snapshot->rssi,
            snapshot->internet_ok ? "PASS" : "UNAVAILABLE");
        lv_label_set_text(s_footer, "OK: SETUP   HOLD OK: BACK");
        break;
    case WIFI_PORTAL_RECONNECTING:
        lv_label_set_text_fmt(
            s_detail, "%s\nSignal lost (reason %u)\nRetrying automatically...",
            snapshot->target_ssid, snapshot->disconnect_reason);
        break;
    case WIFI_PORTAL_FAILED:
        if (snapshot->target_ssid[0] != '\0') {
            lv_label_set_text_fmt(
                s_detail,
                "%s\nReason %u\nCheck password in phone page.",
                snapshot->target_ssid, snapshot->disconnect_reason);
        } else {
            lv_label_set_text_fmt(
                s_detail, "Startup error: %s\nHold OK to return.",
                esp_err_to_name(snapshot->error));
        }
        break;
    case WIFI_PORTAL_STOPPED:
    default:
        lv_label_set_text(s_detail, "Wi-Fi resources released.");
        break;
    }
    if (snapshot->state != WIFI_PORTAL_ONLINE) {
        lv_label_set_text(s_footer, "HOLD OK: BACK");
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_scr == NULL) return;

    wifi_portal_snapshot_t snapshot;
    wifi_portal_get_snapshot(&snapshot);
    if (!s_have_rendered ||
        memcmp(&snapshot, &s_rendered, sizeof(snapshot)) != 0) {
        render_snapshot(&snapshot);
        s_rendered = snapshot;
        s_have_rendered = true;
    }
}

void demo_wifi_enter(void)
{
    s_scr = ui_pixel_screen_create("WI-FI CONNECT");
    lv_obj_t *panel =
        ui_pixel_panel_create(s_scr, 10, 58, 220, 174, UI_PAPER);

    s_status = lv_label_create(panel);
    lv_obj_set_width(s_status, 194);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_SKY_DARK), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(s_status, "STARTING");

    s_detail = lv_label_create(panel);
    lv_obj_set_width(s_detail, 194);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text(s_detail, "Preparing secure setup hotspot...");

    s_footer = lv_label_create(s_scr);
    lv_obj_set_width(s_footer, 220);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_footer, LV_ALIGN_TOP_MID, 0, 244);
    lv_label_set_text(s_footer, "HOLD OK: BACK");

    ui_pixel_mascot_create(s_scr, 101, 272);
    s_have_rendered = false;
    memset(&s_rendered, 0, sizeof(s_rendered));
    s_timer = lv_timer_create(tick, 200, NULL);
    lv_screen_load(s_scr);

    esp_err_t err = wifi_portal_start();
    if (err != ESP_OK) {
        wifi_portal_snapshot_t failed = {
            .state = WIFI_PORTAL_FAILED,
            .error = err,
        };
        render_snapshot(&failed);
    }
}

void demo_wifi_exit(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (!wifi_portal_should_persist()) {
        wifi_portal_stop();
    }
    if (s_scr != NULL) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_status = s_detail = s_footer = NULL;
    s_have_rendered = false;
}

void demo_wifi_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (button != BSP_BTN_OK || event != BSP_BTN_CLICK) return;
    wifi_portal_snapshot_t snapshot;
    wifi_portal_get_snapshot(&snapshot);
    if (snapshot.state == WIFI_PORTAL_ONLINE ||
        snapshot.state == WIFI_PORTAL_FAILED) {
        wifi_portal_begin_setup();
    }
}
