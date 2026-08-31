// components/bsp/src/bsp_wifi_scan.c
// 一次性 Wi-Fi 扫描实现:init -> blocking scan -> get results -> stop/deinit。
// 见 bsp_wifi_scan.h 的线程与生命周期约定。
#include "bsp_wifi_scan.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "bsp_wifi";

esp_err_t bsp_wifi_scan_once(bsp_wifi_ap_t *out, size_t max_out, size_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!out || max_out == 0) return ESP_ERR_INVALID_ARG;

    // --- 幂等准备共享服务。NVS 异常不擦除分区(仓库约定),直接失败返回。 ---
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 不可用: %s;不擦除分区,扫描中止", esp_err_to_name(err));
        return err;
    }
    // 已初始化时这两个接口返回 INVALID_STATE,视为成功。
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_t *sta = NULL;
    bool wifi_inited = false;
    bool wifi_started = false;
    esp_err_t ret = ESP_FAIL;

    sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        ret = err;
        goto cleanup;
    }
    wifi_inited = true;

    // 指纹不落 Wi-Fi 配置,全部放 RAM,避免触碰 NVS 中的 Wi-Fi 键。
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode 失败: %s", esp_err_to_name(err));
        ret = err;
        goto cleanup;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start 失败: %s", esp_err_to_name(err));
        ret = err;
        goto cleanup;
    }
    wifi_started = true;

    {
        // 全信道(channel=0)主动扫描,不指定 SSID;block=true 阻塞至扫描完成。
        wifi_scan_config_t scan_cfg = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "scan_start 失败: %s", esp_err_to_name(err));
            ret = err;
            goto cleanup;
        }
    }

    {
        uint16_t ap_num = 0;
        err = esp_wifi_scan_get_ap_num(&ap_num);
        if (err != ESP_OK) {
            ret = err;
            goto cleanup;
        }
        ESP_LOGI(TAG, "扫描到 %u 个 AP", (unsigned)ap_num);

        if (ap_num > 0) {
            wifi_ap_record_t records[BSP_WIFI_SCAN_MAX];
            uint16_t got = ap_num;
            if (got > BSP_WIFI_SCAN_MAX) got = BSP_WIFI_SCAN_MAX;
            err = esp_wifi_scan_get_ap_records(&got, records);
            if (err != ESP_OK) {
                ret = err;
                goto cleanup;
            }
            size_t n = (got < max_out) ? got : max_out;
            for (size_t i = 0; i < n; i++) {
                memcpy(out[i].bssid, records[i].bssid, sizeof(out[i].bssid));
                out[i].rssi = records[i].rssi;
                out[i].channel = records[i].primary;
            }
            if (out_count) *out_count = n;
        }
    }
    ret = ESP_OK;

cleanup:
    // 无论成败都释放协议栈:scan_stop -> stop -> deinit -> 销毁 netif。
    // 注意:必须在 deinit 前取走扫描结果(上面已拷贝)。
    if (wifi_started) {
        esp_wifi_scan_stop();
        esp_wifi_stop();
    }
    if (wifi_inited) {
        esp_wifi_deinit();
    }
    if (sta) {
        esp_netif_destroy_default_wifi(sta);
    }
    return ret;
}
