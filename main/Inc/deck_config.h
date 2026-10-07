#ifndef MAIN_DECK_CONFIG_H_
#define MAIN_DECK_CONFIG_H_

/* Change these values to match the hotspot created by the computer. */
#define POWER_WIFI_SSID              "myssid"
#define POWER_WIFI_PASSWORD          "mypassword"

/* Streaming: PC sends PDHELLO1 to this port; data returns to its source port. */
#define STREAM_DISCOVERY_PORT       5006
#define STREAM_PEER_LEASE_MS        5000
#define POWER_SAMPLE_PERIOD_MS     100
#define POWER_QUEUE_LENGTH         32
#define DEPTH_FRAME_COUNT          3
/* Match CAM_TEST_FORMAT in camera_spi_mock/Core/Inc/camera_mock.h. */
#define DEPTH_TEST_FORMAT          3U
#define DEPTH_SPI_HZ               10000000  
#define DEPTH_REQUEST_HIGH_MS      8
#define DEPTH_REQUEST_PERIOD_MS    33
#define DEPTH_GPIO_SCK             6
#define DEPTH_GPIO_MISO            2
#define DEPTH_GPIO_MOSI            7
#endif
