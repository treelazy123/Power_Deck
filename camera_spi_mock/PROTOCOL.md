Benchmark update: The frame layout and two-buffer memory figures below document format 1. Current format-2 float and format-3 raw simulator trials use one TX DMA buffer and a small circular RX sink; see ../BENCHMARK_MODES.md.

# STM32F103C8T6 depth camera mock, protocol v1

This is a project-defined mock format, NOT a confirmed camera-deck format.
48 x 48 pixels, row-major, top-left first. All multibyte fields are little-endian.
Each pixel is u16 depth in millimetres followed by u8 status (1 valid; 0 invalid,
with depth zero). The scene has a sloping background, moving near circular
object, and periodically invalid pixels. No floating point or heap allocation.

## Frame layout

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 4 | ASCII DPT1 |
| 4 | 1 | Version = 1 |
| 5 | 1 | Format = 1 (u16 mm + u8 status) |
| 6 | 2 | Header length = 32 |
| 8 | 4 | Frame/request sequence, starts at 0; wraps at 2^32 |
| 12 | 4 | STM32 HAL tick at generation start, ms since boot; wraps |
| 16 | 2 | Width = 48 |
| 18 | 2 | Height = 48 |
| 20 | 4 | Payload length = 6912 |
| 24 | 2 | Depth unit = 1 mm |
| 26 | 2 | Flags = 1 (synthetic) |
| 28 | 4 | Total frame length = 6948 |
| 32 | 6912 | Interleaved depth-low, depth-high, status |
| 6944 | 4 | CRC32 of bytes [0,6944), little-endian |

CRC-32/ISO-HDLC: reflected polynomial 0xEDB88320, init 0xFFFFFFFF,
final XOR 0xFFFFFFFF; compatible with Python zlib.crc32.
Change CAM_WIDTH/CAM_HEIGHT in Core/Inc/camera_mock.h; receiver must agree on
new total length BEFORE clocking. Static assertions enforce DMA/RAM bounds.
An arbitrary 14.8 KB raw frame will NOT fit in this full TX+RX design.

## Wiring and electrical contract

C6 GPIO6/SCK -> PA5, GPIO2/MISO <- PA6, GPIO7/MOSI -> PA7, common GND.
3.3 V signalling. No CS or extra READY. Dedicated bus only. PA4 unused.
SPI mode 0, MSB first, 8-bit words, software NSS selected on STM32.
Start at 1 MHz with short wiring. 10 MHz is a later hardware validation target.
Firmware expects the existing 8 MHz HSE / 72 MHz clock configuration.

## No-CS request/recovery protocol (mandatory on C6)

MOSI doubles as a level request ONLY while SCK is held low. This is NOT a
SPI command byte. C6 must temporarily route MOSI to GPIO, and restore the
SPI output routing without producing spurious clock edges before reading.
Simply calling gpio_set_level while the SPI output still owns MOSI is not
sufficient. Configure CS=-1 and ensure exclusive ownership of the bus.

1. On boot wait at least 100 ms for STM32 initialization. Hold SCK=0.
2. Drive MOSI=0 for at least 2 ms (release the previous request).
3. Drive MOSI=1, still no clocks. STM32 recognizes a sustained high after
   5 ms, aborts/resets the SPI peripheral, creates a new frame and arms DMA.
4. Initially keep this high phase for 50 ms. This is a conservative BENCH
   starting value, not hardware READY or a measured real-time guarantee.
   Inspect g_camera_stats.max_build_ms. The interval must exceed request
   recognition + reset + generation + DMA setup with margin. Do not pause
   STM32 in the debugger while the C6 is clocking.
5. Drive MOSI=0, restore SPI routing with SCK remaining low, and wait 2 ms.
6. Clock EXACTLY 6948 bytes; send 0x00 on MOSI for EVERY byte. Read MISO.
   C6 max_transfer_sz must cover 6948 if using one transaction. Multiple
   zero-command/address/dummy-phase chunks are also allowed; STM32 keeps
   one DMA transfer armed for the ENTIRE frame. No extra clocks or high-MOSI
   request pulses between chunks. Finish within 500 ms of DMA arm.
7. Stop SCK low, leave MOSI low at least 2 ms. Validate magic, header/total
   length, sequence and CRC. Discard invalid frames. Repeat from step 3
   (or step 2 if pin idle level is uncertain).

On truncated frames, master reboot, CRC/alignment errors: stop clocks, do
steps 2-6 again. The long MOSI-high request resets the shift register and DMA
so even a partial-byte transfer can be abandoned. A stuck-high request only
creates one frame; a new request requires MOSI low first. A 500 ms timeout
releases a stalled DMA but does not automatically start another frame.
If the STM32 is reset DURING a request/read, discard the frame and repeat
a fresh low/high request after boot. Extra clocks or noise during request
violate this protocol; CRC detects corruption, not repairs it.

Frame rate is MASTER-PACED. This firmware does not autonomously sample at
30 fps. At 1 MHz the wire time alone is 55.584 ms per frame. The initial
50 ms preparation interval is for functional bring-up, not 30 fps.
At 10 MHz wire time is 5.5584 ms: to target 30 fps, shorten preparation only
after measuring max_build_ms and full-cycle timing (prefer Release), and
schedule requests every 33.33 ms. Hardware CS/READY remains the better
option when reliable low-latency operation is required.

## Implementation / debug

Main calls CameraMock_Init after generated SPI init and CameraMock_Poll
continuously. HAL and generated DMA mapping remain intact. Init overrides
SCK/MOSI input pulldowns, software SSI=0, and IRQ priorities (SysTick 0,
SPI/DMA 2). RX DMA completion is the frame-end event; the application
replaces HAL's RX completion callback to avoid polling BSY in the ISR.
No code generation edits outside USER CODE sections in main.c.
CMakeLists.txt explicitly adds Core/Src/camera_mock.c.

Watch g_camera_stats: requests, completed, interrupted, timeouts, errors,
last_hal_error, last_build_ms, max_build_ms, last_frame_id, bad_dummy_frames,
state (0 waiting; 1 armed/receiving). completed means locally clocked out,
NOT confirmed received or CRC-validated by the C6.

Two 6948-byte static DMA buffers = 13896 bytes. Debug link result: RAM
15768/20480 bytes (includes linker heap/stack reservations), flash 10688/65536.
No UART logging or RTOS. The host must not write commands during data phase.

## Build and verification

Use CMake Debug/Release presets and the configured GNU Arm toolchain.
Run `python tools/test_depth_protocol.py` for decoder corruption/length tests.
Run `python tools/decode_depth.py capture.bin depth.pgm` on a SINGLE raw SPI
frame. PGM contains big-endian 16-bit millimetres, invalid depth zero; viewers
may require intensity scaling. No transport wrapper is included in this file.
These host tests do not validate hardware SPI timing or DMA execution.

Bench checklist: known frame CRC; 1000 consecutive frames; interrupted
read then new request; C6 reboot; STM32 reboot; overlong clocks rejected;
wrong MOSI bytes counted; timeout then recovery; sustained clock-rate test.
