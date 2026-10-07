#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "Inc/i2c.h"
#include "Inc/ina226.h"
#include "Inc/init.h"
#include "Inc/wifi_sta.h"
#include "Inc/deck_config.h"

bool power_deck_init(void)
{
    printf("Hello Power Deck!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s%s%s, ",
            CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi" : "",
           ((chip_info.features & CHIP_FEATURE_WIFI_BGN) && (chip_info.features & (CHIP_FEATURE_BT | CHIP_FEATURE_BLE))) ? "/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           ((chip_info.features & CHIP_FEATURE_BT) && (chip_info.features & CHIP_FEATURE_BLE)) ? "/" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return false;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    i2c_init(I2C_CONTROLLER_0, 19, 18); // SDA=GPIO19, SCL=GPIO18
    if (i2c_probe(I2C_CONTROLLER_0, INA226_SLAVE_ADDRESS) != ESP_OK) {
        printf("INA226 not found at 0x40\n");
        return false;
    }
    printf("I2C OK: INA226 found at 0x40\n");

    return ina226_init(I2C_CONTROLLER_0);
}
