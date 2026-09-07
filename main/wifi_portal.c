#include "wifi_portal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "demo_radio.h"
#include "wifi_session.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "dns_server.h"

#define GOT_IP_BIT          BIT0
#define DISCONNECTED_BIT    BIT1
#define CONNECT_REQUEST_BIT BIT2
#define STOP_REQUEST_BIT    BIT3
#define STOPPED_BIT         BIT4

#define CONNECTION_TIMEOUT_MS 30000
#define HEARTBEAT_MS           30000
#define HEALTH_CHECK_MS       300000
#define HEALTH_FAILURE_LIMIT       3
#define MAX_SCAN_RESULTS           20
#define STOP_TIMEOUT_MS         20000

#define PORTAL_URL     "http://192.168.4.1"
#define DNS_TEST_HOST  "example.com"
#define HTTPS_TEST_URL "https://example.com/"

extern const char portal_start[] asm("_binary_wifi_portal_html_start");
extern const char portal_end[] asm("_binary_wifi_portal_html_end");

static const char *TAG = "wifi_portal";

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_config_mutex;
static SemaphoreHandle_t s_snapshot_mutex;
static SemaphoreHandle_t s_scan_mutex;
static TaskHandle_t s_worker;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static httpd_handle_t s_http_server;
static dns_server_handle_t s_dns_server;
static wifi_config_t s_pending_config;
static char s_pending_api_key[WIFI_SESSION_API_KEY_MAX + 1];
static wifi_portal_snapshot_t s_snapshot;
static bool s_resume_only;

typedef struct {
    char body[1024];
    char encoded_ssid[128];
    char encoded_password[256];
    char encoded_api_key[WIFI_SESSION_API_KEY_MAX * 3 + 1];
    char ssid[33];
    char password[65];
    char api_key[WIFI_SESSION_API_KEY_MAX + 1];
} connect_request_buffers_t;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_wifi_initialized;
static bool s_wifi_started;

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *cursor = data;
    while (size-- > 0) {
        *cursor++ = 0;
    }
}

static void free_connect_request_buffers(connect_request_buffers_t *buffers)
{
    if (buffers == NULL) return;
    secure_zero(buffers, sizeof(*buffers));
    free(buffers);
}

static void snapshot_set_state(wifi_portal_state_t state, esp_err_t error)
{
    if (s_snapshot_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_snapshot.state = state;
    s_snapshot.error = error;
    xSemaphoreGive(s_snapshot_mutex);
}

static void snapshot_set_disconnect_reason(uint8_t reason)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_snapshot.disconnect_reason = reason;
    xSemaphoreGive(s_snapshot_mutex);
}

static void snapshot_set_target(const char *ssid)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    snprintf(s_snapshot.target_ssid, sizeof(s_snapshot.target_ssid), "%s", ssid);
    s_snapshot.disconnect_reason = 0;
    s_snapshot.internet_ok = false;
    s_snapshot.ip[0] = '\0';
    xSemaphoreGive(s_snapshot_mutex);
}

static void snapshot_set_scan_count(uint16_t count)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_snapshot.access_point_count = count;
    xSemaphoreGive(s_snapshot_mutex);
}

static void snapshot_set_online(const esp_netif_ip_info_t *ip_info)
{
    wifi_ap_record_t access_point = {0};
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        rssi = access_point.rssi;
    }

    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    snprintf(s_snapshot.ip, sizeof(s_snapshot.ip), IPSTR, IP2STR(&ip_info->ip));
    s_snapshot.rssi = rssi;
    s_snapshot.state = WIFI_PORTAL_ONLINE;
    s_snapshot.error = ESP_OK;
    xSemaphoreGive(s_snapshot_mutex);
}

static void snapshot_set_internet(bool healthy)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_snapshot.internet_ok = healthy;
    s_snapshot.state = WIFI_PORTAL_ONLINE;
    xSemaphoreGive(s_snapshot_mutex);
}

static void snapshot_clear(void)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    secure_zero(&s_snapshot, sizeof(s_snapshot));
    s_snapshot.state = WIFI_PORTAL_STOPPED;
    xSemaphoreGive(s_snapshot_mutex);
}

void wifi_portal_get_snapshot(wifi_portal_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    if (s_snapshot_mutex == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = WIFI_PORTAL_STOPPED;
        return;
    }
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    *snapshot = s_snapshot;
    xSemaphoreGive(s_snapshot_mutex);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        snapshot_set_disconnect_reason(event->reason);
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        xEventGroupSetBits(s_events, DISCONNECTED_BIT);
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        snapshot_set_online(&event->ip_info);
        xEventGroupClearBits(s_events, DISCONNECTED_BIT);
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

static void get_setup_ssid(char *ssid, size_t ssid_size)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(ssid, ssid_size, "WIFI_SETUP_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
}

static void get_setup_password(char *password, size_t password_size)
{
#if CONFIG_WIFI_PORTAL_FIXED_TEST_PASSWORD
    snprintf(password, password_size, "%s", "88888888");
    ESP_LOGW(TAG, "TEST MODE: fixed setup credential enabled");
#else
    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for (size_t index = 0; index + 1 < password_size; index++) {
        password[index] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    password[password_size - 1] = '\0';
#endif
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool url_decode(const char *input, char *output, size_t output_size)
{
    size_t used = 0;
    while (*input != '\0') {
        uint8_t decoded;
        if (*input == '+') {
            decoded = ' ';
            input++;
        } else if (*input == '%') {
            if (input[1] == '\0' || input[2] == '\0') return false;
            int high = hex_value(input[1]);
            int low = hex_value(input[2]);
            if (high < 0 || low < 0) return false;
            decoded = (uint8_t)((high << 4) | low);
            input += 3;
        } else {
            decoded = (uint8_t)*input++;
        }
        if (decoded == 0 || used + 1 >= output_size) return false;
        output[used++] = (char)decoded;
    }
    output[used] = '\0';
    return true;
}

static esp_err_t send_json(httpd_req_t *request,
                           const char *status,
                           const char *json)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t result = httpd_resp_sendstr(request, json);
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return result;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t result = httpd_resp_send(
        request, portal_start, portal_end - portal_start);
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return result;
}

static esp_err_t networks_get_handler(httpd_req_t *request)
{
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return send_json(request, "503 Service Unavailable",
                         "{\"error\":\"scan_busy\"}");
    }

    snapshot_set_state(WIFI_PORTAL_SCANNING, ESP_OK);
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 40,
            .max = 80,
        },
    };
    esp_wifi_scan_stop();
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_scan_stop();
        err = esp_wifi_scan_start(&scan_config, true);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        snapshot_set_state(WIFI_PORTAL_WAITING, err);
        ESP_LOGE(TAG, "Portal scan start failed: %s", esp_err_to_name(err));
        return send_json(request, "503 Service Unavailable",
                         "{\"error\":\"scan_failed\"}");
    }

    uint16_t available = 0;
    err = esp_wifi_scan_get_ap_num(&available);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        snapshot_set_state(WIFI_PORTAL_WAITING, err);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"scan_count_failed\"}");
    }

    uint16_t count = available < MAX_SCAN_RESULTS ?
                         available : MAX_SCAN_RESULTS;
    wifi_ap_record_t *records =
        calloc(count > 0 ? count : 1, sizeof(*records));
    if (records == NULL) {
        xSemaphoreGive(s_scan_mutex);
        snapshot_set_state(WIFI_PORTAL_WAITING, ESP_ERR_NO_MEM);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"out_of_memory\"}");
    }

    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        free(records);
        xSemaphoreGive(s_scan_mutex);
        snapshot_set_state(WIFI_PORTAL_WAITING, err);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"scan_results_failed\"}");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = root != NULL ?
                          cJSON_AddArrayToObject(root, "networks") : NULL;
    if (networks == NULL) {
        cJSON_Delete(root);
        free(records);
        xSemaphoreGive(s_scan_mutex);
        snapshot_set_state(WIFI_PORTAL_WAITING, ESP_ERR_NO_MEM);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"out_of_memory\"}");
    }

    for (uint16_t index = 0; index < count; index++) {
        if (records[index].ssid[0] == '\0') continue;
        cJSON *network = cJSON_CreateObject();
        if (network == NULL) break;
        cJSON_AddStringToObject(network, "ssid",
                                (const char *)records[index].ssid);
        cJSON_AddNumberToObject(network, "rssi", records[index].rssi);
        cJSON_AddBoolToObject(network, "open",
                              records[index].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
    }
    free(records);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        xSemaphoreGive(s_scan_mutex);
        snapshot_set_state(WIFI_PORTAL_WAITING, ESP_ERR_NO_MEM);
        return send_json(request, "500 Internal Server Error",
                         "{\"error\":\"out_of_memory\"}");
    }

    snapshot_set_scan_count(count);
    snapshot_set_state(WIFI_PORTAL_WAITING, ESP_OK);
    ESP_LOGI(TAG, "Portal scan found %u access points", (unsigned)count);
    xSemaphoreGive(s_scan_mutex);
    esp_err_t result = send_json(request, "200 OK", json);
    free(json);
    return result;
}

static const char *portal_state_name(wifi_portal_state_t state)
{
    switch (state) {
    case WIFI_PORTAL_SCANNING: return "scanning";
    case WIFI_PORTAL_CONNECTING: return "connecting";
    case WIFI_PORTAL_CHECKING: return "checking";
    case WIFI_PORTAL_ONLINE: return "online";
    case WIFI_PORTAL_RECONNECTING: return "reconnecting";
    case WIFI_PORTAL_FAILED: return "failed";
    default: return "waiting";
    }
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    wifi_portal_snapshot_t snapshot;
    wifi_portal_get_snapshot(&snapshot);
    char json[96];
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"reason\":%u}",
             portal_state_name(snapshot.state),
             snapshot.disconnect_reason);
    return send_json(request, "200 OK", json);
}

static esp_err_t connect_post_handler(httpd_req_t *request)
{
    connect_request_buffers_t *buffers = calloc(1, sizeof(*buffers));
    if (buffers == NULL) {
        return send_json(request, "503 Service Unavailable",
                         "{\"error\":\"out_of_memory\"}");
    }

    if (request->content_len <= 0 ||
        request->content_len >= sizeof(buffers->body)) {
        free_connect_request_buffers(buffers);
        return send_json(
            request, "413 Payload Too Large",
            "{\"error\":\"invalid_request_size\"}");
    }

    size_t received = 0;
    while (received < request->content_len) {
        int result = httpd_req_recv(
            request, buffers->body + received,
            request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (result <= 0) {
            free_connect_request_buffers(buffers);
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    buffers->body[received] = '\0';

    bool valid =
        httpd_query_key_value(
            buffers->body, "ssid", buffers->encoded_ssid,
            sizeof(buffers->encoded_ssid)) == ESP_OK &&
        httpd_query_key_value(
            buffers->body, "password", buffers->encoded_password,
            sizeof(buffers->encoded_password)) == ESP_OK &&
        httpd_query_key_value(
            buffers->body, "api_key", buffers->encoded_api_key,
            sizeof(buffers->encoded_api_key)) == ESP_OK &&
        url_decode(buffers->encoded_ssid, buffers->ssid,
                   sizeof(buffers->ssid)) &&
        url_decode(buffers->encoded_password, buffers->password,
                   sizeof(buffers->password)) &&
        url_decode(buffers->encoded_api_key, buffers->api_key,
                   sizeof(buffers->api_key)) &&
        strlen(buffers->ssid) > 0 &&
        strlen(buffers->ssid) <= sizeof(s_pending_config.sta.ssid) &&
        strlen(buffers->password) <=
            sizeof(s_pending_config.sta.password) - 1 &&
        strlen(buffers->api_key) <= WIFI_SESSION_API_KEY_MAX;
    if (!valid) {
        free_connect_request_buffers(buffers);
        return send_json(request, "400 Bad Request",
                         "{\"error\":\"invalid_credentials\"}");
    }

    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    secure_zero(&s_pending_config, sizeof(s_pending_config));
    memcpy(s_pending_config.sta.ssid, buffers->ssid,
           strlen(buffers->ssid));
    memcpy(s_pending_config.sta.password, buffers->password,
           strlen(buffers->password));
    secure_zero(s_pending_api_key, sizeof(s_pending_api_key));
    memcpy(s_pending_api_key, buffers->api_key,
           strlen(buffers->api_key));
    s_pending_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    s_pending_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s_pending_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    s_pending_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    xSemaphoreGive(s_config_mutex);

    snapshot_set_target(buffers->ssid);
    snapshot_set_state(WIFI_PORTAL_CONNECTING, ESP_OK);
    xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
    ESP_LOGI(TAG, "Credentials received for SSID '%s'", buffers->ssid);
    free_connect_request_buffers(buffers);
    return send_json(request, "202 Accepted",
                     "{\"state\":\"connecting\"}");
}

static esp_err_t portal_redirect_handler(httpd_req_t *request,
                                         httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", PORTAL_URL);
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t result = httpd_resp_sendstr(request, "Open Wi-Fi setup");
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return result;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) return err;

    const httpd_uri_t root = {
        .uri = "/*", .method = HTTP_GET, .handler = root_get_handler,
    };
    const httpd_uri_t networks = {
        .uri = "/api/networks",
        .method = HTTP_GET,
        .handler = networks_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t connect = {
        .uri = "/api/connect",
        .method = HTTP_POST,
        .handler = connect_post_handler,
    };

    if ((err = httpd_register_uri_handler(s_http_server, &networks)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_http_server, &status)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_http_server, &connect)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_http_server, &root)) != ESP_OK ||
        (err = httpd_register_err_handler(
             s_http_server, HTTPD_404_NOT_FOUND,
             portal_redirect_handler)) != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return err;
    }
    return ESP_OK;
}

static esp_err_t start_captive_dns(void)
{
    static char portal_uri[] = PORTAL_URL;
    esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t err = esp_netif_dhcps_option(
        s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        portal_uri, strlen(portal_uri));
    if (err != ESP_OK) return err;
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK) return err;

    dns_server_config_t dns_config =
        DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns_server = start_dns_server(&dns_config);
    return s_dns_server != NULL ? ESP_OK : ESP_FAIL;
}

static void stop_portal_services(void)
{
    if (s_dns_server != NULL) {
        stop_dns_server(s_dns_server);
        s_dns_server = NULL;
    }
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

static esp_err_t network_start(void)
{
    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_resume_only) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (s_sta_netif == NULL || (!s_resume_only && s_ap_netif == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init);
    if (err != ESP_OK) return err;
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
        &s_wifi_handler);
    if (err != ESP_OK) return err;
    s_wifi_handler_registered = true;
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL,
        &s_ip_handler);
    if (err != ESP_OK) return err;
    s_ip_handler_registered = true;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    if (s_resume_only) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) return err;
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
        snapshot_set_state(WIFI_PORTAL_CONNECTING, ESP_OK);
        return ESP_OK;
    }

    char setup_ssid[sizeof(s_snapshot.setup_ssid)] = {0};
    char setup_password[sizeof(s_snapshot.setup_password)] = {0};
    get_setup_ssid(setup_ssid, sizeof(setup_ssid));
    get_setup_password(setup_password, sizeof(setup_password));
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    snprintf(s_snapshot.setup_ssid, sizeof(s_snapshot.setup_ssid),
             "%s", setup_ssid);
    snprintf(s_snapshot.setup_password, sizeof(s_snapshot.setup_password),
             "%s", setup_password);
    xSemaphoreGive(s_snapshot_mutex);

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    memcpy(ap_config.ap.ssid, setup_ssid, strlen(setup_ssid));
    ap_config.ap.ssid_len = strlen(setup_ssid);
    memcpy(ap_config.ap.password, setup_password, strlen(setup_password));
    wifi_config_t empty_station_config = {0};

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    secure_zero(setup_password, sizeof(setup_password));
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &empty_station_config);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;

    err = start_captive_dns();
    if (err != ESP_OK) return err;
    err = start_http_server();
    if (err != ESP_OK) return err;

    snapshot_set_state(WIFI_PORTAL_WAITING, ESP_OK);
    ESP_LOGI(TAG, "Setup portal ready: SSID=%s URL=%s",
             setup_ssid, PORTAL_URL);
    return ESP_OK;
}

static void network_cleanup(void)
{
    stop_portal_services();
    if (s_wifi_started) {
        wifi_config_t empty_config = {0};
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &empty_config);
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_handler_registered) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler_registered = false;
    }
    if (s_ip_handler_registered) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler_registered = false;
    }
    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    secure_zero(&s_pending_config, sizeof(s_pending_config));
    secure_zero(s_pending_api_key, sizeof(s_pending_api_key));
    xSemaphoreGive(s_config_mutex);
}

static bool stop_requested(void)
{
    return (xEventGroupGetBits(s_events) & STOP_REQUEST_BIT) != 0;
}

static bool verify_dns(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    int err = getaddrinfo(DNS_TEST_HOST, NULL, &hints, &result);
    if (result != NULL) freeaddrinfo(result);
    return err == 0 && result != NULL;
}

static bool verify_https(void)
{
    esp_http_client_config_t config = {
        .url = HTTPS_TEST_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ?
                     esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return err == ESP_OK && status >= 200 && status < 400;
}

static bool verify_internet(void)
{
    snapshot_set_state(WIFI_PORTAL_CHECKING, ESP_OK);
    bool healthy = verify_dns() && verify_https();
    snapshot_set_internet(healthy);
    ESP_LOGI(TAG, "Internet check: %s", healthy ? "PASS" : "FAIL");
    return healthy;
}

static bool wait_for_connection(void)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_events, GOT_IP_BIT | DISCONNECTED_BIT | STOP_REQUEST_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS));
    return (bits & GOT_IP_BIT) != 0;
}

static bool connect_pending_config(void)
{
    wifi_config_t station_config = {0};
    char api_key[WIFI_SESSION_API_KEY_MAX + 1] = {0};
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    memcpy(&station_config, &s_pending_config, sizeof(station_config));
    memcpy(api_key, s_pending_api_key, sizeof(api_key));
    secure_zero(&s_pending_config, sizeof(s_pending_config));
    secure_zero(s_pending_api_key, sizeof(s_pending_api_key));
    xSemaphoreGive(s_config_mutex);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(s_events, GOT_IP_BIT | DISCONNECTED_BIT);
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    }
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        secure_zero(&station_config, sizeof(station_config));
        secure_zero(api_key, sizeof(api_key));
        snapshot_set_state(WIFI_PORTAL_FAILED, err);
        return false;
    }

    if (!wait_for_connection()) {
        secure_zero(&station_config, sizeof(station_config));
        secure_zero(api_key, sizeof(api_key));
        if (!stop_requested()) snapshot_set_state(WIFI_PORTAL_FAILED, ESP_FAIL);
        return false;
    }
    if (stop_requested()) {
        secure_zero(&station_config, sizeof(station_config));
        secure_zero(api_key, sizeof(api_key));
        return false;
    }

    wifi_session_store_wifi(&station_config);
    wifi_session_store_deepseek_key(api_key);
    secure_zero(&station_config, sizeof(station_config));
    secure_zero(api_key, sizeof(api_key));

    vTaskDelay(pdMS_TO_TICKS(1200));
    stop_portal_services();
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        snapshot_set_state(WIFI_PORTAL_FAILED, err);
        return false;
    }
    ESP_LOGI(TAG, "Provisioning succeeded; setup portal stopped");
    verify_internet();
    return true;
}

static bool reconnect_until_online(void)
{
    static const uint8_t backoff_seconds[] = {1, 2, 4, 8, 15, 30};
    size_t attempt = 0;
    while (!stop_requested()) {
        size_t last =
            sizeof(backoff_seconds) / sizeof(backoff_seconds[0]) - 1;
        uint8_t delay_seconds =
            backoff_seconds[attempt < last ? attempt : last];
        snapshot_set_state(WIFI_PORTAL_RECONNECTING, ESP_OK);
        EventBits_t bits = xEventGroupWaitBits(
            s_events, STOP_REQUEST_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(delay_seconds * 1000U));
        if ((bits & STOP_REQUEST_BIT) != 0) return false;

        xEventGroupClearBits(s_events, GOT_IP_BIT | DISCONNECTED_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK && wait_for_connection()) {
            if (stop_requested()) return false;
            wifi_portal_snapshot_t snapshot;
            wifi_portal_get_snapshot(&snapshot);
            ESP_LOGI(TAG, "Reconnected to '%s'", snapshot.target_ssid);
            verify_internet();
            return true;
        }
        attempt++;
    }
    return false;
}

static void keep_connection_alive(void)
{
    TickType_t last_health_check = xTaskGetTickCount();
    uint8_t health_failures = 0;
    while (!stop_requested()) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, DISCONNECTED_BIT | STOP_REQUEST_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(HEARTBEAT_MS));
        if ((bits & STOP_REQUEST_BIT) != 0) return;
        if ((bits & DISCONNECTED_BIT) != 0) {
            if (!reconnect_until_online()) return;
            last_health_check = xTaskGetTickCount();
            health_failures = 0;
            continue;
        }

        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snapshot_set_online(&ip_info);
        }
        TickType_t now = xTaskGetTickCount();
        if ((now - last_health_check) < pdMS_TO_TICKS(HEALTH_CHECK_MS)) {
            continue;
        }
        last_health_check = now;
        if (verify_internet()) {
            health_failures = 0;
        } else if (++health_failures >= HEALTH_FAILURE_LIMIT) {
            health_failures = 0;
            esp_wifi_disconnect();
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    esp_err_t err = network_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi portal start failed: %s", esp_err_to_name(err));
        network_cleanup();
        snapshot_set_state(WIFI_PORTAL_FAILED, err);
        xEventGroupWaitBits(
            s_events, STOP_REQUEST_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    } else {
        if (s_resume_only) {
            xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
        }
        while (!stop_requested()) {
            EventBits_t bits = xEventGroupWaitBits(
                s_events, CONNECT_REQUEST_BIT | STOP_REQUEST_BIT,
                pdTRUE, pdFALSE, portMAX_DELAY);
            if ((bits & STOP_REQUEST_BIT) != 0) break;
            if ((bits & CONNECT_REQUEST_BIT) != 0 &&
                connect_pending_config()) {
                keep_connection_alive();
                break;
            }
        }
        network_cleanup();
    }

    snapshot_clear();
    s_worker = NULL;
    xEventGroupSetBits(s_events, STOPPED_BIT);
    vTaskDelete(NULL);
}

static esp_err_t wifi_portal_start_internal(bool resume_only)
{
    if (s_worker != NULL || s_events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    s_config_mutex = xSemaphoreCreateMutex();
    s_snapshot_mutex = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();
    if (s_events == NULL || s_config_mutex == NULL ||
        s_snapshot_mutex == NULL || s_scan_mutex == NULL) {
        if (s_events != NULL) vEventGroupDelete(s_events);
        if (s_config_mutex != NULL) vSemaphoreDelete(s_config_mutex);
        if (s_snapshot_mutex != NULL) vSemaphoreDelete(s_snapshot_mutex);
        if (s_scan_mutex != NULL) vSemaphoreDelete(s_scan_mutex);
        s_events = NULL;
        s_config_mutex = s_snapshot_mutex = s_scan_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = WIFI_PORTAL_STARTING;
    secure_zero(&s_pending_config, sizeof(s_pending_config));
    secure_zero(s_pending_api_key, sizeof(s_pending_api_key));
    s_resume_only = resume_only;
    if (resume_only) {
        if (!wifi_session_copy_wifi(&s_pending_config)) {
            vEventGroupDelete(s_events);
            vSemaphoreDelete(s_config_mutex);
            vSemaphoreDelete(s_snapshot_mutex);
            vSemaphoreDelete(s_scan_mutex);
            s_events = NULL;
            s_config_mutex = s_snapshot_mutex = s_scan_mutex = NULL;
            return ESP_ERR_INVALID_STATE;
        }
        snprintf(s_snapshot.target_ssid, sizeof(s_snapshot.target_ssid),
                 "%s", (const char *)s_pending_config.sta.ssid);
    }
    BaseType_t created = xTaskCreate(
        worker_task, "wifi_portal", 8192, NULL, 4, &s_worker);
    if (created != pdPASS) {
        vEventGroupDelete(s_events);
        vSemaphoreDelete(s_config_mutex);
        vSemaphoreDelete(s_snapshot_mutex);
        vSemaphoreDelete(s_scan_mutex);
        s_events = NULL;
        s_config_mutex = s_snapshot_mutex = s_scan_mutex = NULL;
        s_worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_portal_start(void)
{
    if (wifi_portal_is_running()) return ESP_OK;
    return wifi_portal_start_internal(wifi_session_has_wifi());
}

esp_err_t wifi_portal_begin_setup(void)
{
    esp_err_t err = wifi_portal_stop();
    if (err != ESP_OK) return err;
    return wifi_portal_start_internal(false);
}

esp_err_t wifi_portal_suspend(void)
{
    return wifi_portal_stop();
}

esp_err_t wifi_portal_resume(void)
{
    if (wifi_portal_is_running()) return ESP_OK;
    if (!wifi_session_has_wifi()) return ESP_ERR_INVALID_STATE;
    return wifi_portal_start_internal(true);
}

bool wifi_portal_is_running(void)
{
    return s_events != NULL;
}

bool wifi_portal_should_persist(void)
{
    if (!wifi_session_has_wifi()) return false;
    wifi_portal_snapshot_t snapshot;
    wifi_portal_get_snapshot(&snapshot);
    return s_resume_only ||
           snapshot.state == WIFI_PORTAL_CHECKING ||
           snapshot.state == WIFI_PORTAL_ONLINE ||
           snapshot.state == WIFI_PORTAL_RECONNECTING;
}

esp_err_t wifi_portal_stop(void)
{
    if (s_events == NULL) return ESP_OK;
    xEventGroupSetBits(s_events, STOP_REQUEST_BIT);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, STOPPED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(STOP_TIMEOUT_MS));
    if ((bits & STOPPED_BIT) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for Wi-Fi worker cleanup");
        return ESP_ERR_TIMEOUT;
    }

    vEventGroupDelete(s_events);
    vSemaphoreDelete(s_config_mutex);
    vSemaphoreDelete(s_snapshot_mutex);
    vSemaphoreDelete(s_scan_mutex);
    s_events = NULL;
    s_config_mutex = s_snapshot_mutex = s_scan_mutex = NULL;
    return ESP_OK;
}
