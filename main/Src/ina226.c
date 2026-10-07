#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Inc/i2c.h"
#include "Inc/ina226.h"

bool ina226_init(uint8_t i2c_master_port)
{
    uint16_t manufacturer, die;

    if (i2c_read_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_MANUFACTURER_ID, &manufacturer) != ESP_OK ||
        i2c_read_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_DIE_ID, &die) != ESP_OK)
        return false;

    printf("Manufacturer ID: 0x%04X\n", manufacturer);
    printf("Die ID Register: 0x%04X\n", die);
    if (manufacturer != 0x5449 || (die & 0xFFF0) != 0x2260) {
        printf("INA226 identity check failed\n");
        return false;
    }

    if (i2c_write_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_CFG_REG, 0x8000) != ESP_OK)
        return false;
    vTaskDelay(pdMS_TO_TICKS(10) + 1);

    // No ALERT: disable all alert sources. GPIO20 is unused.
    if (i2c_write_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_MASKEN_REG, 0x0000) != ESP_OK ||
        i2c_write_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_CAL_REG, INA226_CALIBRATION) != ESP_OK ||
        i2c_write_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_CFG_REG, 0x4127) != ESP_OK)
        return false;

    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    printf("INA226 ready: continuous measurement, 10mOhm, 1mA/LSB\n");
    return true;
}

float ina226_voltage(uint8_t i2c_master_port)
{
    uint16_t data;
    if (i2c_read_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_BUS_VOLT_REG, &data) != ESP_OK)
        return NAN;
    return data * 0.00125f;
}

float ina226_current(uint8_t i2c_master_port)
{
    uint16_t data;
    if (i2c_read_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_CURRENT_REG, &data) != ESP_OK)
        return NAN;
    return (int16_t)data * INA226_CURRENT_LSB;
}

float ina226_power(uint8_t i2c_master_port)
{
    uint16_t data;
    if (i2c_read_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_POWER_REG, &data) != ESP_OK)
        return NAN;
    return data * INA226_POWER_LSB;
}

float ina226_shunt_voltage(uint8_t i2c_master_port)
{
    uint16_t data;
    if (i2c_read_short(i2c_master_port, INA226_SLAVE_ADDRESS, INA226_SHUNT_VOLT_REG, &data) != ESP_OK)
        return NAN;
    return (int16_t)data * 0.0025f;
}
