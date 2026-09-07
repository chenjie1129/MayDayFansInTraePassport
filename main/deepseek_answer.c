#include "deepseek_answer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "wifi_portal.h"
#include "wifi_session.h"

#define REQUEST_BIT BIT0
#define STOP_BIT    BIT1
#define STOPPED_BIT BIT2

#define NETWORK_TIMEOUT_MS 30000
#define REQUEST_TIMEOUT_MS 60000
#define STOP_TIMEOUT_MS    65000
#define RESPONSE_CAPACITY   4096

#define DEEPSEEK_URL   "https://api.deepseek.com/chat/completions"
#define DEEPSEEK_MODEL "deepseek-v4-flash"

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} response_buffer_t;

static const char *TAG = "deepseek_answer";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static deepseek_answer_snapshot_t s_snapshot;
static uint8_t s_requested_deck;
static EventGroupHandle_t s_events;
static TaskHandle_t s_worker;
static char s_api_key[WIFI_SESSION_API_KEY_MAX + 1];

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *cursor = data;
    while (size-- > 0) {
        *cursor++ = 0;
    }
}

static void set_state(deepseek_answer_state_t state,
                      deepseek_answer_error_t error,
                      int http_status,
                      int answer_index)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.state = state;
    s_snapshot.error = error;
    s_snapshot.http_status = http_status;
    s_snapshot.answer_index = answer_index;
    portEXIT_CRITICAL(&s_lock);
}

void deepseek_answer_get_snapshot(deepseek_answer_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

static bool stop_requested(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & STOP_BIT) != 0;
}

static bool wait_for_network(void)
{
    if (!wifi_portal_is_running() &&
        wifi_portal_resume() != ESP_OK) {
        return false;
    }

    set_state(DEEPSEEK_ANSWER_CONNECTING, DEEPSEEK_ERROR_NONE, 0, -1);
    TickType_t started = xTaskGetTickCount();
    while (!stop_requested()) {
        wifi_portal_snapshot_t network;
        wifi_portal_get_snapshot(&network);
        if (network.state == WIFI_PORTAL_ONLINE) {
            set_state(DEEPSEEK_ANSWER_READY,
                      DEEPSEEK_ERROR_NONE, 0, -1);
            return true;
        }
        if ((xTaskGetTickCount() - started) >=
            pdMS_TO_TICKS(NETWORK_TIMEOUT_MS)) {
            return false;
        }
        EventBits_t bits = xEventGroupWaitBits(
            s_events, STOP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(250));
        if ((bits & STOP_BIT) != 0) return false;
    }
    return false;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    response_buffer_t *response = event->user_data;
    size_t available = response->capacity - response->length - 1;
    if ((size_t)event->data_len > available) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->data + response->length,
           event->data, (size_t)event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool parse_answer_index(const char *response, int *answer_index)
{
    bool valid = false;
    cJSON *root = cJSON_Parse(response);
    cJSON *choices = root != NULL ?
                         cJSON_GetObjectItemCaseSensitive(root, "choices") : NULL;
    cJSON *choice = cJSON_IsArray(choices) ?
                        cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice != NULL ?
                         cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    cJSON *content = message != NULL ?
                         cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        cJSON *payload = cJSON_Parse(content->valuestring);
        cJSON *answer = payload != NULL ?
                            cJSON_GetObjectItemCaseSensitive(payload, "answer") :
                            NULL;
        if (cJSON_IsNumber(answer) &&
            answer->valueint >= 0 && answer->valueint < 16) {
            *answer_index = answer->valueint;
            valid = true;
        }
        cJSON_Delete(payload);
    }
    cJSON_Delete(root);
    return valid;
}

static bool is_http_timeout(esp_err_t error)
{
    return error == ESP_ERR_TIMEOUT ||
           error == ESP_ERR_HTTP_CONNECTING ||
           error == ESP_ERR_HTTP_WRITE_DATA ||
           error == ESP_ERR_HTTP_EAGAIN ||
           error == ESP_ERR_HTTP_READ_TIMEOUT;
}

static deepseek_answer_error_t request_answer(uint8_t deck,
                                              int *answer_index,
                                              int *http_status)
{
    static const char *const deck_names[] = {
        "classic", "office-worker", "programmer",
    };
    char body[640];
    int written = snprintf(
        body, sizeof(body),
        "{\"model\":\"" DEEPSEEK_MODEL "\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"Act as an oracle. Choose one "
        "answer index from 0 through 15. Return JSON only in the exact form "
        "{\\\"answer\\\":N}.\"},"
        "{\"role\":\"user\",\"content\":\"deck=%s nonce=%08lx\"}],"
        "\"response_format\":{\"type\":\"json_object\"},"
        "\"thinking\":{\"type\":\"disabled\"},"
        "\"max_tokens\":32,\"temperature\":1.3}",
        deck_names[deck % 3], (unsigned long)esp_random());
    if (written < 0 || (size_t)written >= sizeof(body)) {
        return DEEPSEEK_ERROR_MEMORY;
    }

    response_buffer_t response = {
        .data = calloc(1, RESPONSE_CAPACITY),
        .capacity = RESPONSE_CAPACITY,
    };
    if (response.data == NULL) return DEEPSEEK_ERROR_MEMORY;

    char authorization[WIFI_SESSION_API_KEY_MAX + 8] = {0};
    snprintf(authorization, sizeof(authorization), "Bearer %s", s_api_key);
    esp_http_client_config_t config = {
        .url = DEEPSEEK_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        secure_zero(authorization, sizeof(authorization));
        free(response.data);
        return DEEPSEEK_ERROR_MEMORY;
    }

    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    *http_status = err == ESP_OK ?
                       esp_http_client_get_status_code(client) : 0;
    if (err != ESP_OK) {
        int tls_error = 0;
        int tls_flags = 0;
        int socket_errno = esp_http_client_get_errno(client);
        esp_http_client_get_and_clear_last_tls_error(
            client, &tls_error, &tls_flags);
        ESP_LOGE(TAG,
                 "HTTP transport failed: err=%s (0x%x) errno=%d "
                 "tls=0x%x flags=0x%x",
                 esp_err_to_name(err), (unsigned)err, socket_errno,
                 (unsigned)tls_error, (unsigned)tls_flags);
    }
    esp_http_client_cleanup(client);
    secure_zero(authorization, sizeof(authorization));

    deepseek_answer_error_t result = DEEPSEEK_ERROR_NONE;
    if (is_http_timeout(err)) {
        result = DEEPSEEK_ERROR_TIMEOUT;
    } else if (err != ESP_OK || response.overflow) {
        result = DEEPSEEK_ERROR_HTTP;
    } else if (*http_status == 401 || *http_status == 403) {
        result = DEEPSEEK_ERROR_AUTH;
    } else if (*http_status == 429) {
        result = DEEPSEEK_ERROR_RATE_LIMIT;
    } else if (*http_status < 200 || *http_status >= 300) {
        result = DEEPSEEK_ERROR_HTTP;
    } else if (!parse_answer_index(response.data, answer_index)) {
        result = DEEPSEEK_ERROR_RESPONSE;
    }

    secure_zero(response.data, response.capacity);
    free(response.data);
    return result;
}

static void worker_task(void *arg)
{
    (void)arg;
    if (!wait_for_network()) {
        set_state(DEEPSEEK_ANSWER_ERROR, DEEPSEEK_ERROR_WIFI, 0, -1);
    }

    while (!stop_requested()) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, REQUEST_BIT | STOP_BIT,
            pdTRUE, pdFALSE, portMAX_DELAY);
        if ((bits & STOP_BIT) != 0) break;
        if ((bits & REQUEST_BIT) == 0 || stop_requested()) continue;
        if (!wait_for_network()) {
            if (!stop_requested()) {
                set_state(DEEPSEEK_ANSWER_ERROR,
                          DEEPSEEK_ERROR_WIFI, 0, -1);
            }
            continue;
        }

        uint8_t deck;
        portENTER_CRITICAL(&s_lock);
        deck = s_requested_deck;
        portEXIT_CRITICAL(&s_lock);
        set_state(DEEPSEEK_ANSWER_REQUESTING,
                  DEEPSEEK_ERROR_NONE, 0, -1);

        int answer_index = -1;
        int http_status = 0;
        deepseek_answer_error_t request_error =
            request_answer(deck, &answer_index, &http_status);
        if (request_error == DEEPSEEK_ERROR_NONE) {
            set_state(DEEPSEEK_ANSWER_RESULT, DEEPSEEK_ERROR_NONE,
                      http_status, answer_index);
            ESP_LOGI(TAG, "DeepSeek answer received: deck=%u index=%d",
                     (unsigned)deck, answer_index);
        } else {
            set_state(DEEPSEEK_ANSWER_ERROR, request_error,
                      http_status, -1);
            ESP_LOGE(TAG, "DeepSeek request failed: error=%d http=%d",
                     request_error, http_status);
        }
    }

    secure_zero(s_api_key, sizeof(s_api_key));
    set_state(DEEPSEEK_ANSWER_STOPPED, DEEPSEEK_ERROR_NONE, 0, -1);
    s_worker = NULL;
    xEventGroupSetBits(s_events, STOPPED_BIT);
    vTaskDelete(NULL);
}

esp_err_t deepseek_answer_start(void)
{
    if (s_worker != NULL || s_events != NULL) return ESP_ERR_INVALID_STATE;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.answer_index = -1;
    if (!wifi_session_has_wifi()) {
        s_snapshot.state = DEEPSEEK_ANSWER_NEEDS_WIFI;
        return ESP_OK;
    }
    if (!wifi_session_has_deepseek_key()) {
        s_snapshot.state = DEEPSEEK_ANSWER_NEEDS_API_KEY;
        return ESP_OK;
    }
    wifi_config_t ignored_wifi = {0};
    if (!wifi_session_copy(
            &ignored_wifi, s_api_key, sizeof(s_api_key))) {
        secure_zero(&ignored_wifi, sizeof(ignored_wifi));
        s_snapshot.state = DEEPSEEK_ANSWER_NEEDS_WIFI;
        return ESP_ERR_INVALID_STATE;
    }
    secure_zero(&ignored_wifi, sizeof(ignored_wifi));

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        secure_zero(s_api_key, sizeof(s_api_key));
        set_state(DEEPSEEK_ANSWER_ERROR, DEEPSEEK_ERROR_MEMORY, 0, -1);
        return ESP_ERR_NO_MEM;
    }
    set_state(DEEPSEEK_ANSWER_CONNECTING, DEEPSEEK_ERROR_NONE, 0, -1);
    if (xTaskCreate(worker_task, "answer_net", 8192, NULL, 4,
                    &s_worker) != pdPASS) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        secure_zero(s_api_key, sizeof(s_api_key));
        set_state(DEEPSEEK_ANSWER_ERROR, DEEPSEEK_ERROR_MEMORY, 0, -1);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t deepseek_answer_request(uint8_t deck)
{
    deepseek_answer_snapshot_t snapshot;
    deepseek_answer_get_snapshot(&snapshot);
    if (s_events == NULL || s_worker == NULL) return ESP_ERR_INVALID_STATE;
    if (snapshot.state != DEEPSEEK_ANSWER_READY &&
        snapshot.state != DEEPSEEK_ANSWER_RESULT &&
        snapshot.state != DEEPSEEK_ANSWER_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_requested_deck = deck;
    portEXIT_CRITICAL(&s_lock);
    set_state(DEEPSEEK_ANSWER_REQUESTING, DEEPSEEK_ERROR_NONE, 0, -1);
    xEventGroupSetBits(s_events, REQUEST_BIT);
    return ESP_OK;
}

esp_err_t deepseek_answer_stop(void)
{
    if (s_events == NULL) {
        set_state(DEEPSEEK_ANSWER_STOPPED, DEEPSEEK_ERROR_NONE, 0, -1);
        return ESP_OK;
    }

    xEventGroupSetBits(s_events, STOP_BIT);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, STOPPED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(STOP_TIMEOUT_MS));
    if ((bits & STOPPED_BIT) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for Answer network cleanup");
        return ESP_ERR_TIMEOUT;
    }
    vEventGroupDelete(s_events);
    s_events = NULL;
    return ESP_OK;
}
