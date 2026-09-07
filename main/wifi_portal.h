#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_PORTAL_STOPPED = 0,
    WIFI_PORTAL_STARTING,
    WIFI_PORTAL_WAITING,
    WIFI_PORTAL_SCANNING,
    WIFI_PORTAL_CONNECTING,
    WIFI_PORTAL_CHECKING,
    WIFI_PORTAL_ONLINE,
    WIFI_PORTAL_RECONNECTING,
    WIFI_PORTAL_FAILED,
} wifi_portal_state_t;

typedef struct {
    wifi_portal_state_t state;
    esp_err_t error;
    uint8_t disconnect_reason;
    uint16_t access_point_count;
    int8_t rssi;
    bool internet_ok;
    char setup_ssid[24];
    char setup_password[13];
    char target_ssid[33];
    char ip[16];
} wifi_portal_snapshot_t;

esp_err_t wifi_portal_start(void);
esp_err_t wifi_portal_begin_setup(void);
esp_err_t wifi_portal_suspend(void);
esp_err_t wifi_portal_resume(void);
esp_err_t wifi_portal_stop(void);
bool wifi_portal_is_running(void);
bool wifi_portal_should_persist(void);
void wifi_portal_get_snapshot(wifi_portal_snapshot_t *snapshot);
