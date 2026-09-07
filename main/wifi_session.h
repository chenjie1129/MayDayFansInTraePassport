#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_wifi.h"

#define WIFI_SESSION_API_KEY_MAX 128

void wifi_session_store_wifi(const wifi_config_t *config);
void wifi_session_store_deepseek_key(const char *api_key);
bool wifi_session_has_wifi(void);
bool wifi_session_has_deepseek_key(void);
bool wifi_session_copy_wifi(wifi_config_t *config);
bool wifi_session_copy(wifi_config_t *config,
                       char *api_key,
                       size_t api_key_size);
