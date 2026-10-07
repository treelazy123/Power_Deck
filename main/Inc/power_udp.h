#ifndef MAIN_POWER_UDP_H_
#define MAIN_POWER_UDP_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sequence;
    int64_t timestamp_ms;
    float voltage_v;
    float current_a;
    float power_w;
} power_sample_t;

bool power_udp_init(void);
bool power_udp_send(const power_sample_t *sample, uint32_t session,
                    uint32_t peer_ipv4_be, uint16_t peer_port,
                    int *send_errno);
void power_udp_deinit(void);

#endif
