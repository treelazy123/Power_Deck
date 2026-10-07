#ifndef MAIN_INA226_H_
#define MAIN_INA226_H_

#include <stdbool.h>
#include <stdint.h>

#define INA226_SLAVE_ADDRESS     0x40
#define INA226_CFG_REG           0x00
#define INA226_SHUNT_VOLT_REG    0x01
#define INA226_BUS_VOLT_REG      0x02
#define INA226_POWER_REG         0x03
#define INA226_CURRENT_REG       0x04
#define INA226_CAL_REG           0x05
#define INA226_MASKEN_REG        0x06
#define INA226_MANUFACTURER_ID   0xFE
#define INA226_DIE_ID            0xFF

// Power Deck: Rshunt=10mOhm, Current_LSB=1mA; CAL=0.00512/(0.01*0.001)=512.
#define INA226_CALIBRATION       512
#define INA226_CURRENT_LSB       0.001f
#define INA226_POWER_LSB         (25.0f * INA226_CURRENT_LSB)

bool ina226_init(uint8_t i2c_master_port);
float ina226_voltage(uint8_t i2c_master_port);       // V
float ina226_current(uint8_t i2c_master_port);       // A, signed
float ina226_power(uint8_t i2c_master_port);         // W
float ina226_shunt_voltage(uint8_t i2c_master_port); // mV, signed
// A failed measurement returns NAN, not a fabricated zero.

#endif
