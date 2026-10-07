#include <stdio.h>
#include "driver/i2c.h"
#include "Inc/i2c.h"

void i2c_init(uint8_t i2c_master_port, uint8_t sda_io_num, uint8_t scl_io_num)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = scl_io_num,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    ESP_ERROR_CHECK(i2c_param_config(i2c_master_port, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(i2c_master_port, I2C_MODE_MASTER, 0, 0, 0));
}

esp_err_t i2c_probe(uint8_t i2c_master_port, uint8_t address)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_master_port, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t i2c_write_short(uint8_t i2c_master_port, uint8_t address, uint8_t command, uint16_t data)
{
    uint8_t buffer[] = {command, (uint8_t)(data >> 8), (uint8_t)data};
    esp_err_t ret = i2c_master_write_to_device(i2c_master_port, address, buffer, sizeof(buffer),
                                            pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret != ESP_OK) printf("i2c_write_short failed: register 0x%02X\n", command);
    return ret;
}

esp_err_t i2c_read_short(uint8_t i2c_master_port, uint8_t address, uint8_t command, uint16_t *data)
{
    uint8_t buffer[2];
    // Repeated START, then two bytes with final NACK; high byte comes first.
    esp_err_t ret = i2c_master_write_read_device(i2c_master_port, address, &command, 1,
                                               buffer, 2, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret == ESP_OK) *data = ((uint16_t)buffer[0] << 8) | buffer[1];
    else printf("i2c_read_short failed: register 0x%02X\n", command);
    return ret;
}
