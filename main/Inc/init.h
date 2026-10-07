#ifndef MAIN_INIT_H_
#define MAIN_INIT_H_

#include <stdbool.h>

// Print startup information, initialize I2C and INA226. Call once at startup.
bool power_deck_init(void);

#endif
