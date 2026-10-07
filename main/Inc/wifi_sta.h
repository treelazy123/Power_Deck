#ifndef MAIN_WIFI_STA_H_
#define MAIN_WIFI_STA_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_STA_DEFAULT_MAXIMUM_RETRY 5

// Initialize NVS, WiFi station mode, and wait until connected or retries fail.
bool wifi_sta_init(const char *ssid, const char *password);

// Same as wifi_sta_init(), with an explicit retry limit.
bool wifi_sta_init_with_retry(const char *ssid, const char *password, uint8_t maximum_retry);

// Disconnect and stop the WiFi station if it was started.
esp_err_t wifi_sta_deinit(void);

bool wifi_sta_start(const char *ssid, const char *password);
bool wifi_sta_connected(void);
#endif
