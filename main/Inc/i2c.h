#ifndef MAIN_I2C_H_
#define MAIN_I2C_H_

#include <stdint.h>
#include "esp_err.h"

#define I2C_CONTROLLER_0          0
#define I2C_MASTER_FREQ_HZ        100000
#define I2C_MASTER_TIMEOUT_MS     100

void i2c_init(uint8_t i2c_master_port, uint8_t sda_io_num, uint8_t scl_io_num);
esp_err_t i2c_probe(uint8_t i2c_master_port, uint8_t address);
esp_err_t i2c_write_short(uint8_t i2c_master_port, uint8_t address, uint8_t command, uint16_t data);
esp_err_t i2c_read_short(uint8_t i2c_master_port, uint8_t address, uint8_t command, uint16_t *data);

#endif
