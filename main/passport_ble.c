#include "passport_ble.h"

#include "demo_radio.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include <string.h>

static const char *TAG = "passport_ble";

typedef struct {
    SemaphoreHandle_t synced;
    SemaphoreHandle_t operation_done;
    SemaphoreHandle_t host_stopped;
    passport_payload_t local;
    passport_ble_result_t *result;
    uint8_t other_tribe_bits[PASSPORT_CROWD_BITMAP_BYTES];
    uint8_t addr_type;
    int sync_error;
} ble_window_ctx_t;

static ble_window_ctx_t *s_ctx;

static bool peer_seen(const passport_ble_result_t *result, uint16_t id)
{
    for (uint8_t i = 0; i < result->peer_count; i++) {
        if (result->peers[i].anonymous_id == id) return true;
    }
    return false;
}

static bool mark_id(uint8_t bits[PASSPORT_CROWD_BITMAP_BYTES], uint16_t id)
{
    uint16_t mixed = (uint16_t)(id * 40503u);
    uint16_t slot = (uint16_t)((mixed ^ (mixed >> 6)) & 1023u);
    uint8_t mask = (uint8_t)(1u << (slot & 7u));
    uint8_t *byte = &bits[slot >> 3];
    if ((*byte & mask) != 0) return false;
    *byte |= mask;
    return true;
}

static void observe_advertisement(const struct ble_gap_disc_desc *disc)
{
    if (!s_ctx || !s_ctx->result || !disc) return;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0 ||
        !fields.mfg_data || fields.mfg_data_len < 2) {
        return;
    }

    uint16_t company_id = (uint16_t)fields.mfg_data[0] |
                          ((uint16_t)fields.mfg_data[1] << 8);
    passport_payload_t peer;
    if (!passport_mfg_decode(&peer, company_id, fields.mfg_data + 2,
                             fields.mfg_data_len - 2) ||
        peer.anonymous_id == s_ctx->local.anonymous_id) {
        return;
    }

    passport_feedback_t route =
        passport_feedback_route(s_ctx->local.tribe_code, &peer);
    if (route == PASSPORT_FEEDBACK_OTHER_TRIBE) {
        if (mark_id(s_ctx->other_tribe_bits, peer.anonymous_id) &&
            s_ctx->result->other_tribe_count != UINT16_MAX) {
            s_ctx->result->other_tribe_count++;
        }
        return;
    }

    bool unique = passport_crowd_observe(&s_ctx->result->crowd,
                                         peer.anonymous_id, peer.icon_index);
    if (!unique || peer_seen(s_ctx->result, peer.anonymous_id)) return;
    if (s_ctx->result->peer_count < PASSPORT_BLE_NORMAL_PEER_MAX) {
        s_ctx->result->peers[s_ctx->result->peer_count++] = peer;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!s_ctx || !event) return 0;
    if (event->type == BLE_GAP_EVENT_DISC) {
        observe_advertisement(&event->disc);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE ||
               event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        xSemaphoreGive(s_ctx->operation_done);
    }
    return 0;
}

static void on_reset(int reason)
{
    if (s_ctx) {
        s_ctx->sync_error = reason;
        xSemaphoreGive(s_ctx->synced);
    }
}

static void on_sync(void)
{
    if (!s_ctx) return;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_ctx->addr_type);
    s_ctx->sync_error = rc;
    xSemaphoreGive(s_ctx->synced);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_ctx && s_ctx->host_stopped) xSemaphoreGive(s_ctx->host_stopped);
    nimble_port_freertos_deinit();
}

static int run_advertise(uint32_t duration_ms)
{
    uint8_t payload[PASSPORT_PAYLOAD_STAGE3_LEN];
    uint8_t mfg[2 + PASSPORT_PAYLOAD_STAGE3_LEN];
    if (passport_payload_encode(payload, sizeof(payload), &s_ctx->local,
                                sizeof(payload)) != sizeof(payload)) {
        return BLE_HS_EINVAL;
    }
    mfg[0] = (uint8_t)(PASSPORT_COMPANY_ID & 0xffu);
    mfg[1] = (uint8_t)(PASSPORT_COMPANY_ID >> 8);
    memcpy(mfg + 2, payload, sizeof(payload));

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = mfg;
    fields.mfg_data_len = sizeof(mfg);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ctx->addr_type, NULL, (int32_t)duration_ms,
                           &params, gap_event, NULL);
    if (rc != 0) return rc;
    return xSemaphoreTake(s_ctx->operation_done,
                          pdMS_TO_TICKS(duration_ms + 500)) == pdTRUE
               ? 0 : BLE_HS_ETIMEOUT;
}

static int run_scan(uint32_t duration_ms)
{
    struct ble_gap_disc_params params = { 0 };
    params.itvl = 0x40;
    params.window = 0x30;
    params.passive = 1;
    params.filter_duplicates = 0;
    params.disable_observer_mode = 0;
    int rc = ble_gap_disc(s_ctx->addr_type, (int32_t)duration_ms,
                          &params, gap_event, NULL);
    if (rc != 0) return rc;
    return xSemaphoreTake(s_ctx->operation_done,
                          pdMS_TO_TICKS(duration_ms + 500)) == pdTRUE
               ? 0 : BLE_HS_ETIMEOUT;
}

static void delete_sync_objects(ble_window_ctx_t *ctx)
{
    if (ctx->synced) vSemaphoreDelete(ctx->synced);
    if (ctx->operation_done) vSemaphoreDelete(ctx->operation_done);
    if (ctx->host_stopped) vSemaphoreDelete(ctx->host_stopped);
}

esp_err_t passport_ble_window(const passport_payload_t *local, bool hidden,
                              passport_ble_result_t *result)
{
    if (!local || !result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    passport_crowd_reset(&result->crowd);

    ble_window_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.local = *local;
    ctx.result = result;
    ctx.synced = xSemaphoreCreateBinary();
    ctx.operation_done = xSemaphoreCreateBinary();
    ctx.host_stopped = xSemaphoreCreateBinary();
    if (!ctx.synced || !ctx.operation_done || !ctx.host_stopped) {
        delete_sync_objects(&ctx);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) {
        delete_sync_objects(&ctx);
        return err;
    }
    err = nimble_port_init();
    if (err != ESP_OK) {
        delete_sync_objects(&ctx);
        return err;
    }

    s_ctx = &ctx;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    int rc = 0;
    if (xSemaphoreTake(ctx.synced, pdMS_TO_TICKS(1500)) != pdTRUE) {
        rc = BLE_HS_ETIMEOUT;
    } else if (ctx.sync_error != 0) {
        rc = ctx.sync_error;
    } else if (hidden) {
        rc = run_scan(4400);
    } else {
        for (int round = 0; round < 2 && rc == 0; round++) {
            rc = run_advertise(700);
            if (rc == 0) rc = run_scan(1500);
        }
    }

    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    int stop_rc = nimble_port_stop();
    if (stop_rc == 0) {
        xSemaphoreTake(ctx.host_stopped, pdMS_TO_TICKS(1000));
        nimble_port_deinit();
    } else {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d", stop_rc);
        if (rc == 0) rc = stop_rc;
    }
    s_ctx = NULL;
    delete_sync_objects(&ctx);

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE window failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}
