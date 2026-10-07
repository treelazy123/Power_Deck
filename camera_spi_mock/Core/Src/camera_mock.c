#include "camera_mock.h"
#include <string.h>

_Static_assert(CAM_FRAME_BYTES <= 65535U, "HAL DMA length overflow");
_Static_assert(sizeof(float) == 4U, "float32 required");
/* F103C8 has 20 KiB SRAM: a full RX frame would not fit beside raw TX. */
#define RX_SCRATCH_BYTES 128U
static SPI_HandleTypeDef *port;
static uint8_t tx[CAM_FRAME_BYTES] __attribute__((aligned(4)));
static uint8_t rx_scratch[RX_SCRATCH_BYTES] __attribute__((aligned(4)));
volatile CameraMockStats g_camera_stats;
static volatile uint8_t tx_done, failed;
static volatile uint32_t tx_done_at;
static uint8_t active, tracking_high, request_latched;
static uint32_t high_since, armed_at, sequence;

#if defined(__GNUC__)
#define CAM_FAST __attribute__((optimize("O2")))
#else
#define CAM_FAST
#endif
static void CAM_FAST put16(uint8_t *p, uint16_t x)
{ p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void CAM_FAST put32(uint8_t *p, uint32_t x)
{ for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8U*i)); }
/* CRC-32/ISO-HDLC, compatible with zlib.crc32. */
static uint32_t CAM_FAST crc32(const uint8_t *p, uint32_t n)
{
  static const uint32_t nibble[16]={0x00000000U,0x1db71064U,0x3b6e20c8U,0x26d930acU,
    0x76dc4190U,0x6b6b51f4U,0x4db26158U,0x5005713cU,
    0xedb88320U,0xf00f9344U,0xd6d6a3e8U,0xcb61b38cU,
    0x9b64c2b0U,0x86d3d2d4U,0xa00ae278U,0xbdbdf21cU};
  uint32_t c=0xffffffffU;
  while(n--) {
    c^=*p++;
    c=(c>>4)^nibble[c&15U];
    c=(c>>4)^nibble[c&15U];
  }
  return c^0xffffffffU;
}
static void CAM_FAST build_frame(uint32_t id, uint32_t tick)
{
  memset(tx,0,CAM_HEADER_BYTES);
  memcpy(tx,"DPT1",4);
  tx[4]=1; tx[5]=CAM_TEST_FORMAT;
  put16(tx+6,CAM_HEADER_BYTES);
  put32(tx+8,id); put32(tx+12,tick);
  put16(tx+16,CAM_WIDTH); put16(tx+18,CAM_HEIGHT);
  put32(tx+20,CAM_PAYLOAD_BYTES);
  put16(tx+24,CAM_TEST_FORMAT==1U ? 1U : 0U);
  put16(tx+26,1U); /* synthetic test data */
  put32(tx+28,CAM_FRAME_BYTES);
#if CAM_TEST_FORMAT == 3U
  for(uint32_t i=0;i<CAM_PAYLOAD_BYTES;++i)
    tx[CAM_HEADER_BYTES+i]=(uint8_t)((i*17U+id*13U)&0xffU);
#else
  unsigned center=id%CAM_WIDTH;
  for(unsigned y=0;y<CAM_HEIGHT;++y) {
    for(unsigned x=0;x<CAM_WIDTH;++x) {
      unsigned i=y*CAM_WIDTH+x;
      int dx=(int)x-(int)center, dy=(int)y-(int)(CAM_HEIGHT/2U);
      uint16_t depth=(uint16_t)(1000U+20U*x+10U*y);
      if(dx*dx+dy*dy<36) depth=600;
      uint8_t valid=((i+id)%97U)!=0U;
#if CAM_TEST_FORMAT == 1U
      uint8_t *v=tx+CAM_HEADER_BYTES+3U*i;
      put16(v,valid?depth:0U); v[2]=valid;
#else
      float metres=valid ? (float)depth*0.001f : 0.0f;
      memcpy(tx+CAM_HEADER_BYTES+4U*i,&metres,4U);
#endif
    }
  }
#endif
  put32(tx+CAM_FRAME_BYTES-4U,crc32(tx,CAM_FRAME_BYTES-4U));
}
static void prepare_frame(void)
{
  uint32_t start=HAL_GetTick();
  uint32_t id=sequence++;
  build_frame(id,start);
  g_camera_stats.last_frame_id=id;
  g_camera_stats.last_build_ms=HAL_GetTick()-start;
  if(g_camera_stats.last_build_ms>g_camera_stats.max_build_ms)
    g_camera_stats.max_build_ms=g_camera_stats.last_build_ms;
}
/* Master holds SCK low during reset. Reset clears byte/bit alignment even
 * after a truncated transaction without a hardware CS signal. */
static void reset_spi(void)
{
  uint32_t irq=__get_PRIMASK();
  __disable_irq();
  port->Instance->CR2=0;
  __HAL_SPI_DISABLE(port);
  (void)HAL_DMA_Abort(port->hdmarx);
  (void)HAL_DMA_Abort(port->hdmatx);
  HAL_NVIC_ClearPendingIRQ(DMA1_Channel2_IRQn);
  HAL_NVIC_ClearPendingIRQ(DMA1_Channel3_IRQn);
  HAL_NVIC_ClearPendingIRQ(SPI1_IRQn);
  __HAL_RCC_SPI1_FORCE_RESET();
  __HAL_RCC_SPI1_RELEASE_RESET();
  port->State=HAL_SPI_STATE_READY;
  port->Lock=HAL_UNLOCKED;
  HAL_StatusTypeDef rc=HAL_SPI_Init(port);
  CLEAR_BIT(port->Instance->CR1,SPI_CR1_SSI); /* software NSS selected */
  tx_done=0; failed=0; active=0; g_camera_stats.state=0;
  __set_PRIMASK(irq);
  if(rc!=HAL_OK) Error_Handler();
}
/* TX DMA completes when the last byte is loaded into SPI, potentially before
 * its final bits leave MISO. Poll waits 1 ms before resetting SPI. */
static void tx_dma_complete(DMA_HandleTypeDef *dma)
{
  (void)dma;
  tx_done_at=HAL_GetTick();
  tx_done=1;
}
static void dma_error(DMA_HandleTypeDef *dma)
{ (void)dma; failed=1; }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *spi)
{
  if(spi==port) { g_camera_stats.last_hal_error=spi->ErrorCode; failed=1; }
}
static HAL_StatusTypeDef arm_spi(void)
{
  memset(rx_scratch,0,sizeof(rx_scratch));
  port->ErrorCode=HAL_SPI_ERROR_NONE;
  port->State=HAL_SPI_STATE_BUSY_TX_RX;
  port->hdmarx->XferCpltCallback=NULL;
  port->hdmarx->XferHalfCpltCallback=NULL;
  port->hdmarx->XferErrorCallback=dma_error;
  if(HAL_DMA_Start(port->hdmarx,(uint32_t)&port->Instance->DR,
                   (uint32_t)rx_scratch,RX_SCRATCH_BYTES)!=HAL_OK) return HAL_ERROR;
  SET_BIT(port->Instance->CR2,SPI_CR2_RXDMAEN);
  port->hdmatx->XferCpltCallback=tx_dma_complete;
  port->hdmatx->XferHalfCpltCallback=NULL;
  port->hdmatx->XferErrorCallback=dma_error;
  if(HAL_DMA_Start_IT(port->hdmatx,(uint32_t)tx,
                      (uint32_t)&port->Instance->DR,CAM_FRAME_BYTES)!=HAL_OK) return HAL_ERROR;
  __HAL_DMA_DISABLE_IT(port->hdmatx,DMA_IT_HT);
  SET_BIT(port->Instance->CR2,SPI_CR2_TXDMAEN|SPI_CR2_ERRIE);
  __HAL_SPI_ENABLE(port);
  armed_at=HAL_GetTick(); active=1; g_camera_stats.state=1;
  return HAL_OK;
}
void CameraMock_Init(SPI_HandleTypeDef *spi)
{
  port=spi;
  if(port->Instance!=SPI1 || port->Init.NSS!=SPI_NSS_SOFT ||
     port->Init.Mode!=SPI_MODE_SLAVE || port->Init.DataSize!=SPI_DATASIZE_8BIT)
    Error_Handler();
  HAL_NVIC_SetPriority(SysTick_IRQn,0,0);
  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn,2,0);
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn,2,0);
  HAL_NVIC_SetPriority(SPI1_IRQn,2,0);
  GPIO_InitTypeDef gpio={0};
  gpio.Pin=GPIO_PIN_5|GPIO_PIN_7; gpio.Mode=GPIO_MODE_INPUT;
  gpio.Pull=GPIO_PULLDOWN; HAL_GPIO_Init(GPIOA,&gpio);
  reset_spi();
  port->hdmarx->Init.Mode=DMA_CIRCULAR;
  port->hdmarx->Init.MemInc=DMA_MINC_ENABLE;
  if(HAL_DMA_Init(port->hdmarx)!=HAL_OK) Error_Handler();
  prepare_frame();
}
void CameraMock_Poll(void)
{
  uint32_t now=HAL_GetTick();
  if(failed) {
    ++g_camera_stats.errors; reset_spi(); prepare_frame();
  } else if(tx_done && (uint32_t)(now-tx_done_at)>=1U) {
    ++g_camera_stats.completed;
    /* Only the last 128 MOSI bytes remain in this circular sink. */
    for(unsigned i=0;i<RX_SCRATCH_BYTES;++i)
      if(rx_scratch[i]!=0) { ++g_camera_stats.bad_dummy_frames; break; }
    reset_spi(); prepare_frame();
  } else if(active && (uint32_t)(now-armed_at)>=CAM_TRANSFER_TIMEOUT_MS) {
    ++g_camera_stats.timeouts; reset_spi(); prepare_frame();
  }
  /* Sustained MOSI high without clocks is the out-of-band frame request. */
  if(HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_7)==GPIO_PIN_RESET) {
    tracking_high=0; request_latched=0; return;
  }
  if(request_latched) return;
  if(HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_5)!=GPIO_PIN_RESET) {
    tracking_high=0; return;
  }
  if(!tracking_high) { tracking_high=1; high_since=now; return; }
  if((uint32_t)(now-high_since)<CAM_REQUEST_MS) return;
  request_latched=1;
  if(active) ++g_camera_stats.interrupted;
  reset_spi();
  ++g_camera_stats.requests;
  if(arm_spi()!=HAL_OK) {
    ++g_camera_stats.errors; reset_spi(); prepare_frame();
  }
}