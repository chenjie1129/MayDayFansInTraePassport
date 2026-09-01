#pragma once

#include "esp_err.h"
#include "passport_social.h"

#include <stdbool.h>
#include <stdint.h>

#define PASSPORT_BLE_NORMAL_PEER_MAX 11u

typedef struct {
    passport_payload_t peers[PASSPORT_BLE_NORMAL_PEER_MAX];
    uint8_t peer_count;
    uint16_t other_tribe_count;
    passport_crowd_counter_t crowd;
} passport_ble_result_t;

/*
 * Runs one bounded BLE window. NimBLE is initialized and fully deinitialized
 * inside this call. It blocks and must only run in a worker task.
 *
 * Visible mode alternates 700 ms advertising and 1500 ms passive scanning
 * twice. Hidden mode skips advertising and scans for the full window.
 */
esp_err_t passport_ble_window(const passport_payload_t *local, bool hidden,
                              passport_ble_result_t *result);
