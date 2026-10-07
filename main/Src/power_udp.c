#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "Inc/power_udp.h"

static const char *TAG = "power_udp";
static int s_socket = -1;

bool power_udp_init(void)
{
    if (s_socket >= 0) return true;

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        return false;
    }
    ESP_LOGI(TAG, "Power UDP transmitter ready");
    return true;
}

bool power_udp_send(const power_sample_t *sample, uint32_t session,
                    uint32_t peer_ipv4_be, uint16_t peer_port,
                    int *send_errno)
{
    if (send_errno) *send_errno = 0;
    if (!sample || s_socket < 0 || peer_ipv4_be == 0 || peer_port == 0) {
        if (send_errno) *send_errno = EINVAL;
        return false;
    }

    char payload[320];
    int length = snprintf(payload, sizeof(payload),
        "{\"type\":\"power\",\"session\":%" PRIu32
        ",\"seq\":%" PRIu32 ",\"timestamp_ms\":%" PRId64
        ",\"voltage_v\":%.6f,\"current_a\":%.6f,\"power_w\":%.6f}",
        session, sample->sequence, sample->timestamp_ms,
        sample->voltage_v, sample->current_a, sample->power_w);
    if (length < 0 || length >= (int)sizeof(payload)) {
        if (send_errno) *send_errno = EMSGSIZE;
        return false;
    }

    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(peer_port),
        .sin_addr.s_addr = peer_ipv4_be,
    };
    int sent = sendto(s_socket, payload, (size_t)length, 0,
                      (struct sockaddr *)&destination, sizeof(destination));
    if (sent != length) {
        if (send_errno) *send_errno = (sent < 0) ? errno : EIO;
        return false;
    }
    return true;
}

void power_udp_deinit(void)
{
    if (s_socket >= 0) {
        shutdown(s_socket, 0);
        close(s_socket);
        s_socket = -1;
    }
}
