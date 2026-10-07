#ifndef CAMERA_MOCK_H
#define CAMERA_MOCK_H
#include "main.h"
/* 1: u16 mm + status; 2: float32 metres; 3: opaque 14,800-byte raw benchmark. */
#define CAM_TEST_FORMAT 3U
#define CAM_WIDTH 48U
#define CAM_HEIGHT 48U
#define CAM_HEADER_BYTES 32U
#define CAM_RAW_PAYLOAD_BYTES 14764U
#if CAM_TEST_FORMAT == 1U
#define CAM_PAYLOAD_BYTES (CAM_WIDTH * CAM_HEIGHT * 3U)
#elif CAM_TEST_FORMAT == 2U
#define CAM_PAYLOAD_BYTES (CAM_WIDTH * CAM_HEIGHT * 4U)
#elif CAM_TEST_FORMAT == 3U
#define CAM_PAYLOAD_BYTES CAM_RAW_PAYLOAD_BYTES
#else
#error Unsupported CAM_TEST_FORMAT
#endif
#define CAM_FRAME_BYTES (CAM_HEADER_BYTES + CAM_PAYLOAD_BYTES + 4U)
#define CAM_REQUEST_MS 5U
#define CAM_TRANSFER_TIMEOUT_MS 500U

typedef struct {
  uint32_t requests, completed, interrupted, timeouts, errors;
  uint32_t last_hal_error, last_build_ms, max_build_ms, last_frame_id;
  uint32_t bad_dummy_frames;
  uint32_t state; /* 0 waiting, 1 DMA armed/in progress */
} CameraMockStats;
extern volatile CameraMockStats g_camera_stats;
void CameraMock_Init(SPI_HandleTypeDef *spi);
void CameraMock_Poll(void);
#endif