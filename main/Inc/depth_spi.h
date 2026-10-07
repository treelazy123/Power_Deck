#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "deck_config.h"
#define DEPTH_HEADER_BYTES 32U
#define DEPTH_WIDTH 48U
#define DEPTH_HEIGHT 48U
#if DEPTH_TEST_FORMAT == 1U
#define DEPTH_PAYLOAD_BYTES (DEPTH_WIDTH * DEPTH_HEIGHT * 3U)
#elif DEPTH_TEST_FORMAT == 2U
#define DEPTH_PAYLOAD_BYTES (DEPTH_WIDTH * DEPTH_HEIGHT * 4U)
#elif DEPTH_TEST_FORMAT == 3U
#define DEPTH_PAYLOAD_BYTES 14764U
#else
#error Unsupported DEPTH_TEST_FORMAT
#endif
#define DEPTH_FRAME_BYTES (DEPTH_HEADER_BYTES + DEPTH_PAYLOAD_BYTES + 4U)
esp_err_t depth_spi_init(void);
esp_err_t depth_spi_read(uint8_t *frame);
void depth_spi_pause_us(uint64_t us);
typedef enum {
    DEPTH_CHECK_OK = 0,
    DEPTH_CHECK_HEADER,
    DEPTH_CHECK_CRC,
    DEPTH_CHECK_PAYLOAD
} depth_check_t;
depth_check_t depth_frame_check(const uint8_t *frame);
uint32_t depth_u32(const uint8_t *p);