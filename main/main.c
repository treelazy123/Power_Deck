/* SPDX-License-Identifier: CC0-1.0 */
#include "esp_log.h"
#include "Inc/init.h"
#include "Inc/wifi_sta.h"
#include "Inc/deck_config.h"
#include "Inc/stream_tasks.h"
void app_main(void)
{
    bool sensor=power_deck_init();
    if(!sensor) ESP_LOGW("app","INA226 unavailable; depth streaming remains enabled");
    if(!wifi_sta_start(POWER_WIFI_SSID,POWER_WIFI_PASSWORD)) {
        ESP_LOGE("app","WiFi initialization failed"); return;
    }
    stream_tasks_start(sensor);
}
