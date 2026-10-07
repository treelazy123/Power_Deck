# Power Deck

Power Deck 是一套用于同步采集功耗和深度帧、并通过 WiFi 发送到电脑的实验工程。当前工程包含 ESP32-C6 接收与传输固件、STM32F103C8T6 摄像头模拟固件，以及 Windows 上位机。STM32 目前发送的是合成测试数据，并非真实摄像头原生输出格式。

## 系统结构

```text
INA226 ──I²C──> ESP32-C6 power_sample_task ──> 功耗样本队列 ──> power_udp ──┐
                                                                        ├─WiFi/UDP─> 电脑 deck_gui
STM32F103 模拟帧 ──SPI──> ESP32-C6 depth_rx_task ──> 深度帧缓存/队列 ──> depth_udp ─┘
```

`power_sample_task` 每 100 ms 读取 INA226；`depth_rx_task` 按设定周期向 STM32 请求一帧，通过 SPI 接收并校验 DPT1 帧；`net_tx_task` 管理队列和发送次序。功耗 JSON 与深度帧由独立的 `power_udp`、`depth_udp` 模块发送。电脑向 C6 的 UDP 5006 发送 `PDHELLO1`，C6 使用发现到的电脑 IP 和端口单播回传。电脑接收端监听 UDP 5005。

## 仓库内容

| 路径 | 用途 |
|---|---|
| `main/` | ESP-IDF 固件，包括 WiFi、INA226、SPI 接收、UDP 模块和 FreeRTOS 任务 |
| `camera_spi_mock/` | STM32F103C8T6 摄像头模拟固件、CMake 工程和 DPT1 协议说明 |
| `tool/` | Python 接收器、桌面上位机、帧解析器和工具说明 |
| `bench_artifacts/` | 不同测试载荷模式的预编译 C6 与 STM32 固件 |
| `sdkconfig`、根目录 `CMakeLists.txt` | ESP-IDF 工程配置 |

## 硬件与接线

### STM32F103C8T6 到 Power Deck ESP32-C6

| Power Deck / C6 | STM32F103C8T6 | 作用 |
|---|---|---|
| GPIO6 | PA5 / SPI1_SCK | SPI 时钟，C6 主机输出 |
| GPIO2 | PA6 / SPI1_MISO | STM32 数据输出到 C6 |
| GPIO7 | PA7 / SPI1_MOSI | C6 请求电平及 SPI dummy 数据 |
| GND | GND | 共地 |

信号电平为 3.3 V。当前方案没有 CS 或 READY 引脚，SPI 总线应专用，不能同时接入另一个主机。SPI 使用 Mode 0、8 位、MSB first。启动和无 CS 请求恢复时序见 [`camera_spi_mock/PROTOCOL.md`](camera_spi_mock/PROTOCOL.md)。简要来说，C6 在 SCK 保持低电平时先拉低 MOSI，再拉高请求 STM32 准备帧；随后 MOSI 拉低并读取固定长度数据。数据阶段 MOSI 发送零，不发送命令字节。

### INA226 到 Power Deck

固件使用 I²C 控制器 0、GPIO19 SDA、GPIO18 SCL、100 kHz，总线地址为 `0x40`。测量配置按 10 mΩ 分流电阻和 1 mA 电流 LSB 设置。找不到 INA226 时，功耗采集关闭，深度接收仍会启动。

## 当前帧格式

配置必须在两端匹配：

- C6：`main/Inc/deck_config.h` 中的 `DEPTH_TEST_FORMAT`
- STM32：`camera_spi_mock/Core/Inc/camera_mock.h` 中的 `CAM_TEST_FORMAT`

当前代码两端均设置为格式 3，分辨率字段为 48×48，总帧长 14,800 字节。即使选择格式 3，载荷前仍有 DPT1 帧头，末尾仍有 CRC32；“raw”只表示载荷不按像素解释，不能省略帧结构。

| 格式号 | 载荷 | 载荷大小 | DPT1 总长 | 上位机预览 |
|---:|---|---:|---:|---|
| 1 | 每像素 `uint16 深度(mm) + uint8 状态` | 6,912 B | 6,948 B | 支持 |
| 2 | 48×48 个 `float32 深度(m)`，0 表示无效 | 9,216 B | 9,252 B | 支持 |
| 3 | 不透明合成字节载荷 | 14,764 B | 14,800 B | 不支持像素预览，可保存原始帧 |

DPT1 多字节字段为小端序；帧头包含版本、格式号、序号、时间戳、宽高、载荷长度、单位、标志位和总长度，帧尾是 CRC-32/ISO-HDLC。C6 校验帧头、长度和 CRC 后才会发送。

C6 再将完整 DPT1 帧切成最多 1,200 字节的数据片，每片添加 40 字节 PDK1 头后通过 UDP 发送。电脑按会话号、帧序号和片偏移重组，再校验 DPT1。功耗以 JSON datagram 单独发送。

### 更改帧格式

修改格式时，先将 C6 和 STM32 的格式号改成相同值，然后分别重新构建和烧录两端固件。电脑解析器目前仅接受表中三种固定长度和 48×48 布局。增加真实摄像头的分辨率、像素布局或帧长时，必须同步更新 C6 校验、电脑 `tool/src/power_deck_tool/depth_protocol.py` 解析逻辑及预览方式。

`bench_artifacts/` 用于快速复现三种测试载荷的吞吐测试，不是运行上位机所需文件，也不是摄像头驱动。不要混用不同格式的 C6 和 STM32 镜像。测试镜像地址和基准测试方法见下文“吞吐基准测试模式”；优先使用正常构建流程生成当前固件。

## 吞吐基准测试模式

STM32F103C8T6 发送的是合成 DPT1 帧，不是真实摄像头的原始格式。三个格式的载荷和传输规模如下；每个 UDP 分片最多承载 1,200 字节帧数据，另有 40 字节 PDK1 头：

| 格式 | 载荷内容 | DPT1 总长 | UDP 分片数 |
|---:|---|---:|---:|
| 1 | `uint16 深度(mm) + uint8 状态` | 6,948 B | 6 |
| 2 | 48×48 `float32` 米，0 表示无效 | 9,252 B | 8 |
| 3 | 不透明合成 raw 载荷，不作像素解释 | 14,800 B | 13 |

基准测试时，C6 的 `DEPTH_TEST_FORMAT` 与 STM32 的 `CAM_TEST_FORMAT` 必须设置为相同值。源码默认值为格式 2。可供快速复现的配套预编译固件位于 `bench_artifacts/u16_status`、`bench_artifacts/float` 和 `bench_artifacts/raw`；不要混用不同模式的镜像。烧录时 C6 app bin 地址为 `0x10000`，STM32 bin 地址为 `0x08000000`。正常开发优先通过上文构建流程生成当前固件。

电脑接收器会自动识别三种格式：三种模式均保存 `.bin`，格式 1 和 2 另外生成 `.pgm` 深度图。STM32 使用整帧 TX DMA，以及 128 字节循环 RX DMA sink，以便在 20 KiB SRAM 中接收 raw 大帧。

每种模式运行 60 秒并建立连接后，对比 C6 串口中的 depth `ok/err/no_buf/drop/tx` 计数，以及电脑端的 fps、invalid、incomplete。该测试衡量传输容量，不代表真实传感器采集延迟，也不表示 raw 数据具有真实图像语义。

## 配置与构建

### ESP32-C6（ESP-IDF）







需要 ESP-IDF 5.5.5 及其 ESP32-C6 工具链。使用 VS Code 中的 ESP-IDF 扩展操作，不需要在命令行执行 C6 配置或构建命令：

1. 在 VS Code 打开仓库根目录，确保 ESP-IDF 扩展已安装并完成工具链配置。
2. 如需切换目标芯片，点击状态栏中的目标芯片选择器（或打开命令面板运行 **ESP-IDF: Set Espressif Device Target**），选择 **ESP32-C6**。
3. 烧录前编辑 `main/Inc/deck_config.h`，设置 2.4 GHz WiFi 的 `POWER_WIFI_SSID` 和 `POWER_WIFI_PASSWORD`。同时，开启电脑端的移动热点，并编写对应的名称、密码和频段。`deck_config.h`还包含 SPI 频率、请求周期和格式选择。
4. 通过 ESP-IDF Explorer 的 **SDK Configuration Editor**（`menuconfig`）检查工程配置；需要修改 Kconfig 选项时在该编辑器中保存配置。
5. 在状态栏或 ESP-IDF Explorer 点击 **Build Project** 构建；点击 **Flash Device** 烧录，按提示选择对应串口；点击 **Monitor Device** 查看串口日志。也可使用状态栏对应的构建、烧录和监视按钮。


**当前配置为格式 3、SPI 10 MHz、请求周期 33 ms、MOSI 请求高电平 8 ms；这些是当前测试配置，实际硬件运行仍需观察错误与帧率。**

串口日志会周期性输出功耗与深度的 `ok/err/drop/tx`、SPI/CRC 错误、丢帧原因、队列峰值、堆空间和任务栈余量。WiFi 尚未连接或电脑接收端未发现时的丢帧也会计数。

### STM32F103C8T6（CMake / GNU Arm）

需要 CMake 3.22+、Ninja 和 GNU Arm Embedded 工具链。工程提供 Debug 与 Release preset：

```powershell
cd camera_spi_mock
cmake --preset Debug
cmake --build --preset Debug
```

生成的 ELF/BIN 位于 `camera_spi_mock/build/Debug/`。可通过 STM32CubeIDE 导入或使用 ST-Link 烧录 BIN；Flash 起始地址为 `0x08000000`。修改帧格式时，检查 `Core/Inc/camera_mock.h` 中的 `CAM_TEST_FORMAT`，并与 C6 配置保持一致。调试器可观察 `g_camera_stats` 中的请求、完成、超时、错误、帧构建耗时和最大构建耗时。

### Windows 上位机

需要 Python 3.10+、[uv](https://docs.astral.sh/uv/) 和包含 Tkinter 的 Python。进入 `tool/` 目录运行：

```powershell
uv run deck-gui
```

C6 IP 留空时尝试局域网发现；也可以指定地址：

```powershell
uv run deck-gui --board 192.168.137.66
```

终端接收器可代替 GUI：

```powershell
uv run deck-receiver --board 192.168.137.66 --view --save ..\captures
```

GUI 和终端接收器都占用电脑 UDP 5005，不能同时运行。Windows 防火墙需允许本程序接收 UDP 5005；如果广播发现被网络隔离，手动填写 C6 IP。

GUI 显示最近 60 秒的电压、电流、功率曲线、深度预览、帧率和接收统计。开始记录后，在指定目录下创建新文件夹，保存 `power.csv`、`depth.csv`、每帧原始 `.bin`，以及格式 1/2 的 `.pgm` 深度图。格式 3 载荷布局未知，所以只保存 `.bin`，不生成深度预览。

底部“序号缺口 / 估算丢帧”根据成功重组的 DPT1 帧序号计算：

```text
估算丢帧率 = 序号缺口 / (已收到帧数 + 序号缺口) × 100%
```

统计按设备 IP 和 C6 session 分开；最近的乱序帧会在有限窗口内修正。该估算只覆盖本次 session 首个与最新有效帧之间的序号范围，不计开始前或结束后的未知帧，也不能区分 C6 主动丢帧和 WiFi/UDP 丢帧。GUI 上的 `incomplete` 表示电脑端分片重组超时，不等同于总丢帧率。

目前 GUI 的“帧类型/重新烧录”和“WiFi 指令”是界面预留项，尚未实现烧录与指令发送。

## 调试与验证

- STM32 DPT1 格式说明、SPI 无 CS 请求时序和恢复流程：[`camera_spi_mock/PROTOCOL.md`](camera_spi_mock/PROTOCOL.md)
- 三种载荷模式和预编译固件：见本 README 的“吞吐基准测试模式”章节
- 上位机操作说明：[`tool/GUI_README.md`](tool/GUI_README.md)
- C6 串口：查看 `depth err`、`depth drop`、`drop_reason`、`depth tx` 和堆/栈统计。
- 电脑 GUI：查看序号缺口估算、`invalid`、`incomplete` 和界面跳过数。`evicted`、重组队列峰值等由接收后端统计，但当前 GUI 状态栏不展示。

这套工程目前验证的是 STM32 合成帧到 C6 再到电脑的传输链路。真实摄像头的原生输出格式、采集接口和时序尚未接入；切换真实摄像头前，先确认传感器的分辨率、位宽、深度单位、无效值编码和帧同步方式。
