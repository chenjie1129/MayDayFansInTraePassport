#include "wifi_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_config_t s_wifi_config;
static char s_deepseek_api_key[WIFI_SESSION_API_KEY_MAX + 1];
static bool s_has_wifi;
static bool s_has_deepseek_key;

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *cursor = data;
    while (size-- > 0) {
        *cursor++ = 0;
    }
}

void wifi_session_store_wifi(const wifi_config_t *config)
{
    if (config == NULL || config->sta.ssid[0] == '\0') return;
    portENTER_CRITICAL(&s_lock);
    secure_zero(&s_wifi_config, sizeof(s_wifi_config));
    memcpy(&s_wifi_config, config, sizeof(s_wifi_config));
    s_has_wifi = true;
    portEXIT_CRITICAL(&s_lock);
}

void wifi_session_store_deepseek_key(const char *api_key)
{
    if (api_key == NULL || api_key[0] == '\0') return;
    portENTER_CRITICAL(&s_lock);
    secure_zero(s_deepseek_api_key, sizeof(s_deepseek_api_key));
    snprintf(s_deepseek_api_key, sizeof(s_deepseek_api_key), "%s", api_key);
    s_has_deepseek_key = true;
    portEXIT_CRITICAL(&s_lock);
}

bool wifi_session_has_wifi(void)
{
    portENTER_CRITICAL(&s_lock);
    bool available = s_has_wifi;
    portEXIT_CRITICAL(&s_lock);
    return available;
}

bool wifi_session_has_deepseek_key(void)
{
    portENTER_CRITICAL(&s_lock);
    bool available = s_has_deepseek_key;
    portEXIT_CRITICAL(&s_lock);
    return available;
}

bool wifi_session_copy_wifi(wifi_config_t *config)
{
    if (config == NULL) return false;
    portENTER_CRITICAL(&s_lock);
    bool available = s_has_wifi;
    if (available) memcpy(config, &s_wifi_config, sizeof(*config));
    portEXIT_CRITICAL(&s_lock);
    return available;
}

bool wifi_session_copy(wifi_config_t *config,
                       char *api_key,
                       size_t api_key_size)
{
    if (config == NULL || api_key == NULL || api_key_size == 0) return false;

    portENTER_CRITICAL(&s_lock);
    bool ready = s_has_wifi && s_has_deepseek_key;
    if (ready) {
        memcpy(config, &s_wifi_config, sizeof(*config));
        snprintf(api_key, api_key_size, "%s", s_deepseek_api_key);
    }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}
