#ifndef MAIN_DEPTH_UDP_H_
#define MAIN_DEPTH_UDP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* Owns the streaming UDP socket, PC discovery lease and PDK1 serialization. */
bool depth_udp_init(void);
void depth_udp_poll(bool wifi_connected, uint32_t now_ms,
                    bool *wifi_lost, bool *peer_expired);
bool depth_udp_has_peer(void);
bool depth_udp_peer_endpoint(uint32_t *peer_ipv4_be, uint16_t *peer_port);
bool depth_udp_send_depth_chunk(const uint8_t *frame, uint32_t frame_bytes,
                                uint32_t session, uint32_t seq,
                                uint32_t rx_ms, uint32_t age_ms,
                                uint32_t offset, uint32_t *sent_bytes,
                                int *send_errno);
void depth_udp_deinit(void);

#endif
