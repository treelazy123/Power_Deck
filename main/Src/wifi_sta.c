#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "Inc/wifi_sta.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_wifi_event_group;
static esp_event_handler_instance_t s_wifi_event_any_id;
static esp_event_handler_instance_t s_ip_event_got_ip;
static esp_netif_t *s_wifi_netif;
static uint8_t s_retry_num;
static uint8_t s_maximum_retry = WIFI_STA_DEFAULT_MAXIMUM_RETRY;
static bool s_netif_initialized;
static bool s_event_loop_created;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_auto_reconnect;

static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_auto_reconnect || s_retry_num < s_maximum_retry) {
            esp_wifi_connect();
            if (s_retry_num < UINT8_MAX) s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_sta_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

bool wifi_sta_init_with_retry(const char *ssid, const char *password, uint8_t maximum_retry)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "invalid WiFi credentials");
        return false;
    }

    if (password == NULL) {
        password = "";
    }

    s_maximum_retry = maximum_retry;
    s_retry_num = 0;

    if (wifi_sta_init_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
        return false;
    }

    if (!s_netif_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_netif_initialized = true;
    }

    if (!s_event_loop_created) {
        esp_err_t ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(ret));
            return false;
        }
        s_event_loop_created = true;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "failed to create WiFi event group");
            return false;
        }
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (s_wifi_netif == NULL) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
        if (s_wifi_netif == NULL) {
            ESP_LOGE(TAG, "failed to create WiFi station netif");
            return false;
        }
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_sta_event_handler,
                                                            NULL,
                                                            &s_wifi_event_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_sta_event_handler,
                                                            NULL,
                                                            &s_ip_event_got_ip));
        s_wifi_initialized = true;
    }

    if (s_wifi_started) {
        esp_wifi_disconnect();
        ESP_ERROR_CHECK(esp_wifi_stop());
        s_wifi_started = false;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    ESP_LOGI(TAG, "wifi_sta_init finished.");

    if (s_auto_reconnect) return true;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        return true;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "failed to connect to SSID:%s", ssid);
        return false;
    }

    ESP_LOGE(TAG, "unexpected WiFi event");
    return false;
}

bool wifi_sta_init(const char *ssid, const char *password)
{
    return wifi_sta_init_with_retry(ssid, password, WIFI_STA_DEFAULT_MAXIMUM_RETRY);
}

esp_err_t wifi_sta_deinit(void)
{
    esp_err_t ret = ESP_OK;

    s_auto_reconnect=false;
    if (s_wifi_started) {
        ret = esp_wifi_stop();
        s_wifi_started = false;
    }

    if (s_wifi_initialized) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_event_got_ip);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_any_id);
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }

    if (s_wifi_netif != NULL) {
        esp_netif_destroy(s_wifi_netif);
        s_wifi_netif = NULL;
    }

    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    return ret;
}

bool wifi_sta_start(const char *ssid, const char *password)
{
    s_auto_reconnect=true;
    return wifi_sta_init_with_retry(ssid,password,WIFI_STA_DEFAULT_MAXIMUM_RETRY);
}
bool wifi_sta_connected(void)
{
    return s_wifi_event_group && (xEventGroupGetBits(s_wifi_event_group)&WIFI_CONNECTED_BIT);
}
