# Power Deck 上位机工具

适用于 Windows 的桌面接收器、监控界面和协议工具。需要 Python 3.10+ 和 [uv](https://docs.astral.sh/uv/)。运行时仅依赖 Python 标准库；图形界面使用 Tkinter。

## 从源码目录运行

在本目录中执行：

```powershell
uv run deck-gui
uv run deck-receiver --board 192.168.137.66 --view --save ..\captures
```

`deck-gui` 会打开监控界面，提供实时功耗曲线、深度预览和数据记录功能。C6 IP 留空即可自动发现同一局域网中的 Power Deck；发现后界面会显示设备 IP。也可以手动填写 IP。`deck-receiver` 是终端接收器；省略 `--view` 可在无图形界面的情况下采集数据。两个程序都会监听 UDP 5005，并向 UDP 5006 发送现有的 `PDHELLO1` 发现报文。不要同时运行这两个程序，否则它们会争用 UDP 5005 端口。

## 安装和分发

在本目录中构建 wheel，然后将生成的文件复制到装有 Python 3.10+ 和 uv 的 Windows 电脑：

```powershell
uv build
uv tool install .\dist\power_deck_tools-0.1.0-py3-none-any.whl
deck-gui
deck-receiver --view --save captures
```

也可以将源码目录复制到目标电脑，然后在该目录运行 `uv tool install .`。本项目没有第三方运行时依赖。使用 `deck-gui` 或 `deck-receiver --view` 时，所用 Python 发行版必须包含 Tkinter。

Windows 防火墙需要允许本程序接收 UDP 5005 端口的数据。自动发现使用 UDP 广播，因此电脑和开发板需要位于支持广播互通的同一局域网；若网络禁用了广播，可在界面中手动填写 C6 IP。监控界面会在新的记录目录中保存 `power.csv`、`depth.csv` 和原始帧文件，不会覆盖已有目录。

## 命令选项

运行 `deck-receiver --help` 查看绑定地址、端口、运行时长、预览和采集选项；运行 `deck-gui --help` 查看监控界面的选项。


GUI 底部的“序号缺口 / 估算丢帧”按有效 DPT1 帧的 32 位序号计算：估算丢帧率 = 序号缺口 ÷（已收到帧数 + 序号缺口）。新设备或新 session 会重新开始统计；最近的乱序帧会在有限窗口内修正。此比例只覆盖本 session 中首个和最新有效帧之间的序号范围，不计开始前或结束后的未知帧，也不能区分 C6 主动丢帧和 WiFi/UDP 丢帧。
