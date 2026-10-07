#include <string.h>
#include "Inc/depth_spi.h"
#include "Inc/deck_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "soc/gpio_sig_map.h"
#include "soc/spi_periph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static spi_device_handle_t camera;
static uint8_t *zeros;
static esp_timer_handle_t pause_timer;
static void wake(void *arg) { xTaskNotifyGive((TaskHandle_t)arg); }
static void pause_us(uint64_t us)
{
    ESP_ERROR_CHECK(esp_timer_start_once(pause_timer, us));
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
void depth_spi_pause_us(uint64_t us)
{
    if(us) pause_us(us);
}
uint32_t depth_u32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t u16(const uint8_t *p) { return p[0]|((uint16_t)p[1]<<8); }
depth_check_t depth_frame_check(const uint8_t *p)
{
    if(memcmp(p,"DPT1",4)||p[4]!=1||p[5]!=DEPTH_TEST_FORMAT||
       u16(p+6)!=DEPTH_HEADER_BYTES||
       u16(p+16)!=DEPTH_WIDTH||u16(p+18)!=DEPTH_HEIGHT||
       depth_u32(p+20)!=DEPTH_PAYLOAD_BYTES||
       u16(p+24)!=(DEPTH_TEST_FORMAT==1U?1U:0U)||u16(p+26)!=1U||
       depth_u32(p+28)!=DEPTH_FRAME_BYTES) return DEPTH_CHECK_HEADER;
    static const uint32_t nibble[16]={
        0x00000000U,0x1db71064U,0x3b6e20c8U,0x26d930acU,
        0x76dc4190U,0x6b6b51f4U,0x4db26158U,0x5005713cU,
        0xedb88320U,0xf00f9344U,0xd6d6a3e8U,0xcb61b38cU,
        0x9b64c2b0U,0x86d3d2d4U,0xa00ae278U,0xbdbdf21cU
    };
    uint32_t crc=0xffffffffU;
    for(unsigned i=0;i<DEPTH_FRAME_BYTES-4U;i++) {
        crc^=p[i];
        crc=(crc>>4)^nibble[crc&15U];
        crc=(crc>>4)^nibble[crc&15U];
    }
    if((crc^0xffffffffU)!=depth_u32(p+DEPTH_FRAME_BYTES-4U)) return DEPTH_CHECK_CRC;
#if DEPTH_TEST_FORMAT == 1U
    for(unsigned i=DEPTH_HEADER_BYTES;i<DEPTH_FRAME_BYTES-4U;i+=3U)
        if(p[i+2]>1U || (p[i+2]==0U && u16(p+i)!=0U)) return DEPTH_CHECK_PAYLOAD;
#elif DEPTH_TEST_FORMAT == 2U
    /* Little-endian IEEE-754: reject negative values, NaN and infinity. */
    for(unsigned i=DEPTH_HEADER_BYTES;i<DEPTH_FRAME_BYTES-4U;i+=4U) {
        uint32_t bits=depth_u32(p+i);
        if((bits&0x80000000U) || (bits&0x7f800000U)==0x7f800000U) return DEPTH_CHECK_PAYLOAD;
    }
#endif
    return DEPTH_CHECK_OK;
}
/* SPI uses the GPIO matrix even on native pins. Detach ONLY MOSI; the SPI
 * peripheral keeps SCK low in mode 0. Only this task owns SPI2. */
static void mosi_gpio(int level)
{
    gpio_set_level(DEPTH_GPIO_MOSI,level);
    esp_rom_gpio_connect_out_signal(DEPTH_GPIO_MOSI,SIG_GPIO_OUT_IDX,false,false);
}
esp_err_t depth_spi_init(void)
{
    spi_bus_config_t bus={.mosi_io_num=DEPTH_GPIO_MOSI,.miso_io_num=DEPTH_GPIO_MISO,
        .sclk_io_num=DEPTH_GPIO_SCK,.quadwp_io_num=-1,.quadhd_io_num=-1,
        .max_transfer_sz=DEPTH_FRAME_BYTES,.flags=SPICOMMON_BUSFLAG_GPIO_PINS};
    esp_err_t err=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);
    if(err!=ESP_OK) return err;
    spi_device_interface_config_t device={.clock_speed_hz=DEPTH_SPI_HZ,.mode=0,
        .spics_io_num=-1,.queue_size=1,.command_bits=0,.address_bits=0,.dummy_bits=0};
    err=spi_bus_add_device(SPI2_HOST,&device,&camera);
    if(err!=ESP_OK) return err;
    zeros=heap_caps_calloc(1,DEPTH_FRAME_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!zeros) return ESP_ERR_NO_MEM;
    esp_timer_create_args_t timer={.callback=wake,.arg=xTaskGetCurrentTaskHandle(),.name="spi_pause"};
    err=esp_timer_create(&timer,&pause_timer);
    if(err!=ESP_OK) return err;
    mosi_gpio(0);
    pause_us(150000); /* allow STM32 startup */
    return ESP_OK;
}
esp_err_t depth_spi_read(uint8_t *frame)
{
    mosi_gpio(0); pause_us(2000);
    gpio_set_level(DEPTH_GPIO_MOSI,1); pause_us(DEPTH_REQUEST_HIGH_MS*1000ULL);
    gpio_set_level(DEPTH_GPIO_MOSI,0); pause_us(2000);
    esp_rom_gpio_connect_out_signal(DEPTH_GPIO_MOSI,
        spi_periph_signal[SPI2_HOST].spid_out,false,false);
    /* Previous transmission ends with 0 on MOSI. No command/address/dummy clocks. */
    spi_transaction_t transfer={.length=DEPTH_FRAME_BYTES*8,.rxlength=DEPTH_FRAME_BYTES*8,
        .tx_buffer=zeros,.rx_buffer=frame};
    esp_err_t err=spi_device_transmit(camera,&transfer);
    mosi_gpio(0); pause_us(2000);
    return err;
}
