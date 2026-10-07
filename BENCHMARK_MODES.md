# SPI payload benchmark
STM32F103C8T6 sends synthetic DPT1 frames. This is not the real camera raw format.
Format 1: u16 mm + status, 6948 B, 6 UDP fragments.
Format 2: 48x48 float32 metres, 0 means invalid, 9252 B, 8 UDP fragments.
Format 3: opaque synthetic raw, 14800 B total, 13 UDP fragments. No pixel interpretation.
Select matching CAM_TEST_FORMAT and DEPTH_TEST_FORMAT; source defaults to format 2.
Matching binaries are under bench_artifacts/u16_status, bench_artifacts/float, and bench_artifacts/raw. Flash C6 app bin at 0x10000 and STM32 bin at 0x08000000.
PC receiver autodetects all three formats, saves .bin for each and .pgm for formats 1 and 2.
The STM32 uses full-frame TX DMA and a 128-byte circular RX DMA sink to fit raw frames in 20 KiB SRAM.
Run a 60-second trial for each mode; compare C6 depth ok/err/no_buf/drop/tx and PC fps/invalid/incomplete after connection.
These are transport capacity trials; no real sensor acquisition latency or raw image semantics are represented.
