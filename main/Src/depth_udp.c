#include <errno.h>
#include <string.h>
#include <fcntl.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "Inc/deck_config.h"
#include "Inc/depth_udp.h"

#define UDP_CHUNK_BYTES 1200U
#define PDK1_HEADER_BYTES 40U

static const char *TAG = "depth_udp";
static int s_socket = -1;
static struct sockaddr_in s_peer;
static bool s_have_peer;
static bool s_was_wifi_connected;
static uint32_t s_peer_seen_ms;

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

bool depth_udp_init(void)
{
    if (s_socket >= 0) return true;

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        return false;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(STREAM_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_socket, (struct sockaddr *)&local, sizeof(local)) < 0 ||
        fcntl(s_socket, F_SETFL, O_NONBLOCK) < 0) {
        int saved_errno = errno;
        ESP_LOGE(TAG, "Unable to bind UDP discovery port %u: errno %d",
                 STREAM_DISCOVERY_PORT, saved_errno);
        depth_udp_deinit();
        errno = saved_errno;
        return false;
    }

    s_have_peer = false;
    s_was_wifi_connected = false;
    s_peer_seen_ms = 0;
    ESP_LOGI(TAG, "Listening for PDHELLO1 on UDP %u", STREAM_DISCOVERY_PORT);
    return true;
}

void depth_udp_poll(bool wifi_connected, uint32_t now_ms,
                    bool *wifi_lost, bool *peer_expired)
{
    if (wifi_lost) *wifi_lost = false;
    if (peer_expired) *peer_expired = false;

    if (!wifi_connected) {
        if (s_was_wifi_connected && wifi_lost) *wifi_lost = true;
        s_was_wifi_connected = false;
        s_have_peer = false;
        return;
    }
    s_was_wifi_connected = true;

    if (s_have_peer && (uint32_t)(now_ms - s_peer_seen_ms) > STREAM_PEER_LEASE_MS) {
        s_have_peer = false;
        if (peer_expired) *peer_expired = true;
    }

    if (s_socket < 0) return;
    for (unsigned i = 0; i < 4; ++i) {
        char hello[32];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_socket, hello, sizeof(hello), 0,
                         (struct sockaddr *)&from, &from_len);
        if (n < 0) break;
        if (n != 8 || memcmp(hello, "PDHELLO1", 8) != 0) continue;

        if (!s_have_peer ||
            (from.sin_addr.s_addr == s_peer.sin_addr.s_addr &&
             from.sin_port == s_peer.sin_port)) {
            if (!s_have_peer) {
                ESP_LOGI(TAG, "PC receiver %s:%u", inet_ntoa(from.sin_addr),
                         ntohs(from.sin_port));
            }
            s_peer = from;
            s_peer_seen_ms = now_ms;
            s_have_peer = true;
        }
    }
}

bool depth_udp_has_peer(void)
{
    return s_have_peer;
}

static bool send_packet(const void *data, size_t length, int *send_errno)
{
    if (send_errno) *send_errno = 0;
    if (s_socket < 0 || !s_have_peer) {
        if (send_errno) *send_errno = ENOTCONN;
        return false;
    }
    int sent = sendto(s_socket, data, length, 0,
                      (const struct sockaddr *)&s_peer, sizeof(s_peer));
    if (sent != (int)length) {
        if (send_errno) *send_errno = (sent < 0) ? errno : EIO;
        return false;
    }
    return true;
}

bool depth_udp_peer_endpoint(uint32_t *peer_ipv4_be, uint16_t *peer_port)
{
    if (!s_have_peer || !peer_ipv4_be || !peer_port) return false;
    *peer_ipv4_be = s_peer.sin_addr.s_addr;
    *peer_port = ntohs(s_peer.sin_port);
    return true;
}

bool depth_udp_send_depth_chunk(const uint8_t *frame, uint32_t frame_bytes,
                                uint32_t session, uint32_t seq,
                                uint32_t rx_ms, uint32_t age_ms,
                                uint32_t offset, uint32_t *sent_bytes,
                                int *send_errno)
{
    if (!frame || offset >= frame_bytes || !sent_bytes) {
        if (send_errno) *send_errno = EINVAL;
        return false;
    }
    uint32_t bytes = frame_bytes - offset;
    if (bytes > UDP_CHUNK_BYTES) bytes = UDP_CHUNK_BYTES;

    uint8_t packet[PDK1_HEADER_BYTES + UDP_CHUNK_BYTES] = {0};
    memcpy(packet, "PDK1", 4);
    packet[4] = 1;
    packet[5] = 2;
    put16(packet + 6, PDK1_HEADER_BYTES);
    put32(packet + 8, session);
    put32(packet + 12, seq);
    put32(packet + 16, frame_bytes);
    put32(packet + 20, offset);
    put16(packet + 24, (uint16_t)bytes);
    put16(packet + 26, (uint16_t)(offset / UDP_CHUNK_BYTES));
    put16(packet + 28, (uint16_t)((frame_bytes + UDP_CHUNK_BYTES - 1) / UDP_CHUNK_BYTES));
    put32(packet + 32, rx_ms);
    put32(packet + 36, age_ms);
    memcpy(packet + PDK1_HEADER_BYTES, frame + offset, bytes);

    if (!send_packet(packet, PDK1_HEADER_BYTES + bytes, send_errno)) return false;
    *sent_bytes = bytes;
    return true;
}

void depth_udp_deinit(void)
{
    if (s_socket >= 0) {
        shutdown(s_socket, 0);
        close(s_socket);
        s_socket = -1;
    }
    s_have_peer = false;
}
