#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "Inc/deck_config.h"
#include "Inc/power_udp.h"
#include "Inc/depth_udp.h"
#include "Inc/ina226.h"
#include "Inc/i2c.h"
#include "Inc/wifi_sta.h"
#include "Inc/depth_spi.h"
#include "Inc/stream_tasks.h"
static const char *TAG="stream";
static QueueHandle_t power_q,free_q,ready_q;
static bool sensor_ready;
static uint32_t session;
static TaskHandle_t power_handle,depth_handle,net_handle;
typedef struct { uint8_t *data; uint32_t rx_ms,seq; } Frame;
static Frame frames[DEPTH_FRAME_COUNT];
typedef struct {
    uint32_t power_ok,power_error,power_drop,power_sent;
    uint32_t depth_ok,depth_error,depth_no_buffer,depth_sent,depth_drop,send_error;
    uint32_t depth_spi_error,depth_header_error,depth_crc_error,depth_payload_error;
    uint32_t last_spi_err,last_bad_word0,last_bad_word1;
    uint32_t drop_ready_full,drop_no_wifi,drop_no_receiver,drop_stale,drop_udp;
    uint32_t peer_expired,peer_wifi_lost,last_send_errno;
    uint32_t last_drop_seq,last_drop_age_ms,last_drop_offset;
    uint32_t pool_used,pool_peak,power_peak,ready_peak,spi_us_max;
} Stats;
static Stats stats;
static portMUX_TYPE stats_lock=portMUX_INITIALIZER_UNLOCKED;
#define COUNT(field) do { portENTER_CRITICAL(&stats_lock); ++stats.field; portEXIT_CRITICAL(&stats_lock); } while(0)
typedef enum { DROP_READY_FULL, DROP_NO_WIFI, DROP_NO_RECEIVER, DROP_STALE, DROP_UDP } DropReason;
static void record_drop(DropReason reason, uint32_t seq, uint32_t age_ms, uint32_t offset)
{
    portENTER_CRITICAL(&stats_lock);
    ++stats.depth_drop;
    switch(reason) {
    case DROP_READY_FULL: ++stats.drop_ready_full; break;
    case DROP_NO_WIFI: ++stats.drop_no_wifi; break;
    case DROP_NO_RECEIVER: ++stats.drop_no_receiver; break;
    case DROP_STALE: ++stats.drop_stale; break;
    case DROP_UDP: ++stats.drop_udp; break;
    }
    stats.last_drop_seq=seq;
    stats.last_drop_age_ms=age_ms;
    stats.last_drop_offset=offset;
    portEXIT_CRITICAL(&stats_lock);
}
static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static TickType_t ticks(unsigned ms) { return pdMS_TO_TICKS(ms)+1; }
static void peak(uint32_t *dest,uint32_t value) {
    portENTER_CRITICAL(&stats_lock); if(value>*dest) *dest=value; portEXIT_CRITICAL(&stats_lock);
}
static void release_frame(uint8_t id) {
    portENTER_CRITICAL(&stats_lock); --stats.pool_used; portEXIT_CRITICAL(&stats_lock);
    BaseType_t result=xQueueSend(free_q,&id,0); configASSERT(result==pdTRUE);
}
static void power_sample_task(void *arg)
{
    (void)arg; uint32_t seq=0; TickType_t due=xTaskGetTickCount();
    for(;;) {
        if(sensor_ready) {
            power_sample_t sample={.sequence=seq++,.timestamp_ms=esp_timer_get_time()/1000};
            sample.voltage_v=ina226_voltage(I2C_CONTROLLER_0);
            sample.current_a=ina226_current(I2C_CONTROLLER_0);
            sample.power_w=ina226_power(I2C_CONTROLLER_0);
            if(isfinite(sample.voltage_v)&&isfinite(sample.current_a)&&isfinite(sample.power_w)) {
                COUNT(power_ok);
                if(xQueueSend(power_q,&sample,0)!=pdTRUE) COUNT(power_drop);
                peak(&stats.power_peak,uxQueueMessagesWaiting(power_q));
            } else COUNT(power_error);
        }
        vTaskDelayUntil(&due,pdMS_TO_TICKS(POWER_SAMPLE_PERIOD_MS));
    }
}
static void depth_rx_task(void *arg)
{
    (void)arg; ESP_ERROR_CHECK(depth_spi_init());
    for(;;) {
        int64_t started=esp_timer_get_time(); uint8_t id;
        if(xQueueReceive(free_q,&id,0)==pdTRUE) {
            portENTER_CRITICAL(&stats_lock);
            if(++stats.pool_used>stats.pool_peak) stats.pool_peak=stats.pool_used;
            portEXIT_CRITICAL(&stats_lock);
            esp_err_t err=depth_spi_read(frames[id].data);
            peak(&stats.spi_us_max,(uint32_t)(esp_timer_get_time()-started));
            depth_check_t check=(err==ESP_OK)?depth_frame_check(frames[id].data):DEPTH_CHECK_OK;
            if(err==ESP_OK && check==DEPTH_CHECK_OK) {
                frames[id].seq=depth_u32(frames[id].data+8); frames[id].rx_ms=millis();
                COUNT(depth_ok);
                if(xQueueSend(ready_q,&id,0)!=pdTRUE) {
                    record_drop(DROP_READY_FULL,frames[id].seq,0,0); release_frame(id);
                }
                peak(&stats.ready_peak,uxQueueMessagesWaiting(ready_q));
            } else {
                portENTER_CRITICAL(&stats_lock);
                ++stats.depth_error;
                if(err!=ESP_OK) {
                    ++stats.depth_spi_error;
                    stats.last_spi_err=(uint32_t)err;
                } else {
                    if(check==DEPTH_CHECK_HEADER) ++stats.depth_header_error;
                    else if(check==DEPTH_CHECK_CRC) ++stats.depth_crc_error;
                    else if(check==DEPTH_CHECK_PAYLOAD) ++stats.depth_payload_error;
                    stats.last_bad_word0=depth_u32(frames[id].data);
                    stats.last_bad_word1=depth_u32(frames[id].data+4);
                }
                portEXIT_CRITICAL(&stats_lock);
                release_frame(id);
            }
        } else COUNT(depth_no_buffer);
        int64_t remaining=DEPTH_REQUEST_PERIOD_MS*1000LL-(esp_timer_get_time()-started);
        /* The project tick is 10 ms; its rounding previously added a large
         * fraction of a 33 ms frame period. Use this task's esp_timer here. */
        if(remaining>0) depth_spi_pause_us((uint64_t)remaining);
        else taskYIELD();
    }
}
static void record_send_error(int error)
{
    portENTER_CRITICAL(&stats_lock);
    ++stats.send_error;
    stats.last_send_errno = (uint32_t)error;
    portEXIT_CRITICAL(&stats_lock);
}
static void net_tx_task(void *arg)
{
    (void)arg;
    int active = -1;
    uint32_t offset = 0;
    bool udp_ready = false;
    bool power_ready = false;
    uint32_t next_power_init_ms = 0;

    for (;;) {
        if (!udp_ready) {
            udp_ready = depth_udp_init();
            if (!udp_ready) {
                record_send_error((errno != 0) ? errno : EIO);
                vTaskDelay(ticks(1000));
                continue;
            }
        }

        uint32_t now = millis();
        if (!power_ready && (int32_t)(now - next_power_init_ms) >= 0) {
            power_ready = power_udp_init();
            next_power_init_ms = now + 1000U;
            if (!power_ready) record_send_error((errno != 0) ? errno : EIO);
        }
        bool wifi_connected = wifi_sta_connected();
        bool wifi_lost = false;
        bool peer_expired = false;
        depth_udp_poll(wifi_connected, now, &wifi_lost, &peer_expired);
        if (wifi_lost) COUNT(peer_wifi_lost);
        if (peer_expired) COUNT(peer_expired);
        bool have_peer = depth_udp_has_peer();

        if (!have_peer && active >= 0) {
            Frame *frame = &frames[active];
            record_drop(wifi_connected ? DROP_NO_RECEIVER : DROP_NO_WIFI,
                        frame->seq, millis() - frame->rx_ms, offset);
            release_frame((uint8_t)active);
            active = -1;
        }

        /* At most one power sample precedes each depth fragment. */
        power_sample_t sample;
        if (xQueueReceive(power_q, &sample, 0) == pdTRUE) {
            uint32_t peer_ipv4_be;
            uint16_t peer_port;
            int send_errno = 0;
            if (power_ready && have_peer &&
                depth_udp_peer_endpoint(&peer_ipv4_be, &peer_port) &&
                power_udp_send(&sample, session, peer_ipv4_be, peer_port,
                               &send_errno)) {
                COUNT(power_sent);
            } else {
                if (send_errno) record_send_error(send_errno);
                COUNT(power_drop);
            }
        }

        if (active < 0) {
            uint8_t id;
            if (xQueueReceive(ready_q, &id, 0) == pdTRUE) {
                active = id;
                offset = 0;
            }
        }
        if (active >= 0) {
            Frame *frame = &frames[active];
            uint32_t frame_now = millis();
            uint32_t frame_age = frame_now - frame->rx_ms;
            if (!have_peer || frame_age > 500U) {
                record_drop(!wifi_connected ? DROP_NO_WIFI :
                            (!have_peer ? DROP_NO_RECEIVER : DROP_STALE),
                            frame->seq, frame_age, offset);
                release_frame((uint8_t)active);
                active = -1;
            } else {
                uint32_t sent_bytes = 0;
                int send_errno = 0;
                if (!depth_udp_send_depth_chunk(frame->data, DEPTH_FRAME_BYTES,
                        session, frame->seq, frame->rx_ms, frame_age,
                        offset, &sent_bytes, &send_errno)) {
                    if (send_errno) record_send_error(send_errno);
                    record_drop(DROP_UDP, frame->seq, frame_age, offset);
                    release_frame((uint8_t)active);
                    active = -1;
                } else {
                    offset += sent_bytes;
                    if (offset == DEPTH_FRAME_BYTES) {
                        COUNT(depth_sent);
                        release_frame((uint8_t)active);
                        active = -1;
                    }
                }
            }
        }

        if (active < 0) vTaskDelay(1);
    }
}
static void stats_task(void *arg)
{
    (void)arg;
    for(;;) {
        vTaskDelay(ticks(1000)); Stats s;
        portENTER_CRITICAL(&stats_lock); s=stats; portEXIT_CRITICAL(&stats_lock);
        ESP_LOGI(TAG,"power ok/err/drop/tx=%lu/%lu/%lu/%lu depth ok/err/no_buf/drop/tx=%lu/%lu/%lu/%lu/%lu send_err=%lu pool=%lu peak=%lu power_q_peak=%lu ready_peak=%lu spi_cycle_max_us=%lu heap=%u min=%u dma_block=%u stacks=%u/%u/%u",
            (unsigned long)s.power_ok,(unsigned long)s.power_error,(unsigned long)s.power_drop,(unsigned long)s.power_sent,
            (unsigned long)s.depth_ok,(unsigned long)s.depth_error,(unsigned long)s.depth_no_buffer,(unsigned long)s.depth_drop,(unsigned long)s.depth_sent,
            (unsigned long)s.send_error,(unsigned long)s.pool_used,(unsigned long)s.pool_peak,(unsigned long)s.power_peak,(unsigned long)s.ready_peak,(unsigned long)s.spi_us_max,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
            (unsigned)uxTaskGetStackHighWaterMark(power_handle),(unsigned)uxTaskGetStackHighWaterMark(depth_handle),(unsigned)uxTaskGetStackHighWaterMark(net_handle));
        ESP_LOGI(TAG,"depth_err_detail spi/header/crc/payload=%lu/%lu/%lu/%lu last_spi_err=0x%lx last_bad_first8=%08lx/%08lx",
            (unsigned long)s.depth_spi_error,(unsigned long)s.depth_header_error,
            (unsigned long)s.depth_crc_error,(unsigned long)s.depth_payload_error,
            (unsigned long)s.last_spi_err,(unsigned long)s.last_bad_word0,(unsigned long)s.last_bad_word1);
        ESP_LOGI(TAG,"drop_reason ready_full/no_wifi/no_receiver/stale/udp=%lu/%lu/%lu/%lu/%lu peer_expired=%lu peer_wifi_lost=%lu last_send_errno=%lu last_drop seq/age_ms/offset=%lu/%lu/%lu",
            (unsigned long)s.drop_ready_full,(unsigned long)s.drop_no_wifi,
            (unsigned long)s.drop_no_receiver,(unsigned long)s.drop_stale,
            (unsigned long)s.drop_udp,(unsigned long)s.peer_expired,
            (unsigned long)s.peer_wifi_lost,(unsigned long)s.last_send_errno,
            (unsigned long)s.last_drop_seq,(unsigned long)s.last_drop_age_ms,
            (unsigned long)s.last_drop_offset);
    }
}
void stream_tasks_start(bool power_sensor_ready)
{
    sensor_ready=power_sensor_ready; session=esp_random();
    ESP_LOGI(TAG,"stream_tasks timestamp_fix build=%s %s",__DATE__,__TIME__);
    ESP_LOGI(TAG,"depth benchmark format=%u frame_bytes=%u spi_hz=%u request_high_ms=%u",
        (unsigned)DEPTH_TEST_FORMAT,(unsigned)DEPTH_FRAME_BYTES,
        (unsigned)DEPTH_SPI_HZ,(unsigned)DEPTH_REQUEST_HIGH_MS);
    power_q=xQueueCreate(POWER_QUEUE_LENGTH,sizeof(power_sample_t));
    free_q=xQueueCreate(DEPTH_FRAME_COUNT,sizeof(uint8_t));
    ready_q=xQueueCreate(DEPTH_FRAME_COUNT,sizeof(uint8_t));
    if(!power_q || !free_q || !ready_q) abort();
    for(uint8_t i=0;i<DEPTH_FRAME_COUNT;i++) {
        frames[i].data=heap_caps_malloc(DEPTH_FRAME_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        if(!frames[i].data || xQueueSend(free_q,&i,0)!=pdTRUE) abort();
    }
    if(xTaskCreate(power_sample_task,"power_sample",3072,NULL,5,&power_handle)!=pdPASS) abort();
    if(xTaskCreate(depth_rx_task,"depth_rx",4096,NULL,6,&depth_handle)!=pdPASS) abort();
    if(xTaskCreate(net_tx_task,"net_tx",5120,NULL,4,&net_handle)!=pdPASS) abort();
    if(xTaskCreate(stats_task,"stats",3072,NULL,2,NULL)!=pdPASS) abort();
}
