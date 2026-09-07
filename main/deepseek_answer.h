#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DEEPSEEK_ANSWER_STOPPED = 0,
    DEEPSEEK_ANSWER_NEEDS_WIFI,
    DEEPSEEK_ANSWER_NEEDS_API_KEY,
    DEEPSEEK_ANSWER_CONNECTING,
    DEEPSEEK_ANSWER_READY,
    DEEPSEEK_ANSWER_REQUESTING,
    DEEPSEEK_ANSWER_RESULT,
    DEEPSEEK_ANSWER_ERROR,
} deepseek_answer_state_t;

typedef enum {
    DEEPSEEK_ERROR_NONE = 0,
    DEEPSEEK_ERROR_WIFI,
    DEEPSEEK_ERROR_TIMEOUT,
    DEEPSEEK_ERROR_AUTH,
    DEEPSEEK_ERROR_RATE_LIMIT,
    DEEPSEEK_ERROR_HTTP,
    DEEPSEEK_ERROR_RESPONSE,
    DEEPSEEK_ERROR_MEMORY,
} deepseek_answer_error_t;

typedef struct {
    deepseek_answer_state_t state;
    deepseek_answer_error_t error;
    int answer_index;
    int http_status;
} deepseek_answer_snapshot_t;

esp_err_t deepseek_answer_start(void);
esp_err_t deepseek_answer_request(uint8_t deck);
esp_err_t deepseek_answer_stop(void);
void deepseek_answer_get_snapshot(deepseek_answer_snapshot_t *snapshot);
