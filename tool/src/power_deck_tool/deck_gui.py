#!/usr/bin/env python3
"""Power Deck desktop dashboard: live depth, power charts, and recording."""
import argparse
import ipaddress
import queue
import time
import tkinter as tk
from collections import deque
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from .deck_gui_backend import CaptureRecorder, UdpReceiver

BG = "#0b1220"
PANEL = "#162235"
PANEL_ALT = "#1c2b40"
TEXT = "#eef5ff"
MUTED = "#9cb0c7"
GRID = "#33445c"
TEAL = "#51d6c5"
BLUE = "#74a8ff"
AMBER = "#ffbf69"
RED = "#ff7183"
FORMATS = {"u16 + 状态": (1, "u16_status"),
           "float32 深度": (2, "float"),
           "原始字节帧": (3, "raw")}


def depth_ppm(frame, max_mm=3000):
    """Build a colourized PPM image from a validated 48x48 depth frame."""
    if frame["pixels"] is None:
        return None
    colors = bytearray()
    for depth, valid in frame["pixels"]:
        if not valid:
            colors.extend((9, 17, 30))
            continue
        t = min(1.0, max(0.0, depth / max_mm))
        colors.extend((int(240 * (1-t) + 15),
                       int(190 * (1-abs(2*t-1)) + 30),
                       int(220 * t + 25)))
    return f"P6\n{frame['width']} {frame['height']}\n255\n".encode() + colors


class PowerChart(tk.Canvas):
    def __init__(self, parent):
        super().__init__(parent, bg=PANEL, highlightthickness=0, height=205)
        self.samples = deque(maxlen=900)
        self.bind("<Configure>", lambda _event: self.redraw())

    def add(self, timestamp, sample):
        self.samples.append((timestamp, sample["voltage_v"],
                             sample["current_a"], sample["power_w"]))

    def redraw(self):
        self.delete("all")
        width = max(self.winfo_width(), 350)
        height = max(self.winfo_height(), 205)
        left, right = 74, width-18
        top, bottom = 14, height-20
        slot = (bottom-top)/3
        now = time.monotonic()
        values = [row for row in self.samples if now-row[0] <= 60]
        specs = (("电压", "V", TEAL, 1),
                 ("电流", "A", BLUE, 2),
                 ("功率", "W", AMBER, 3))
        for panel_index, (title, unit, color, field) in enumerate(specs):
            y0 = top + panel_index*slot
            y1 = y0 + slot - 8
            self.create_text(14, y0+8, anchor="nw", text=title,
                             fill=TEXT, font=("Segoe UI", 11, "bold"))
            self.create_text(14, y0+30, anchor="nw", text=unit,
                             fill=MUTED, font=("Segoe UI", 9))
            for fraction in (0, 0.5, 1):
                y = y0 + fraction*(y1-y0)
                self.create_line(left, y, right, y, fill=GRID, dash=(2, 4))
            if not values:
                self.create_text((left+right)/2, (y0+y1)/2,
                                 text="等待功耗数据", fill=MUTED,
                                 font=("Segoe UI", 10))
                continue
            series = [row[field] for row in values]
            lo, hi = min(series), max(series)
            pad = max((hi-lo)*0.15, 0.02 if field == 1 else 0.005)
            lo -= pad
            hi += pad
            self.create_text(right, y0+5, anchor="ne", text=f"{hi:.3f}",
                             fill=MUTED, font=("Consolas", 9))
            self.create_text(right, y1-2, anchor="se", text=f"{lo:.3f}",
                             fill=MUTED, font=("Consolas", 9))
            points = []
            for row in values:
                stamp, value = row[0], row[field]
                x = right - min(60, now-stamp)/60*(right-left)
                y = y1 - (value-lo)/(hi-lo)*(y1-y0)
                points.extend((x, y))
            if len(points) >= 4:
                self.create_line(*points, fill=color, width=2, smooth=False)
            elif points:
                self.create_oval(points[0]-2, points[1]-2,
                                 points[0]+2, points[1]+2, fill=color, outline="")
        self.create_text(left, height-7, anchor="sw", text="−60 s",
                         fill=MUTED, font=("Segoe UI", 9))
        self.create_text(right, height-7, anchor="se", text="现在",
                         fill=MUTED, font=("Segoe UI", 9))


class DeckApp:
    def __init__(self, root, initial_board=""):
        self.root = root
        root.title("Power Deck · 实时监测")
        root.geometry("1260x820")
        root.minsize(1020, 820)
        root.configure(bg=BG)
        self.events = queue.Queue(maxsize=1024)
        self.receiver = None
        self.recorder = None
        self.finishing = []
        self.latest_frame = None
        self.last_packet_at = 0.0
        self.receiver_error = False
        self.board_var = tk.StringVar(value=initial_board)
        self.status_var = tk.StringVar(value="未连接")
        self.stats_var = tk.StringVar(value="深度 0 fps    完整帧 0    功耗样本 0    无效 0    未完成 0")
        self.frame_var = tk.StringVar(value="等待深度帧")
        self.record_var = tk.StringVar(value="未记录")
        self.voltage_var = tk.StringVar(value="— V")
        self.current_var = tk.StringVar(value="— A")
        self.power_var = tk.StringVar(value="— W")
        self.save_root_var = tk.StringVar(value="Documents/PowerDeckCaptures")
        self.folder_var = tk.StringVar(value=self._suggest_folder())
        self.format_var = tk.StringVar(value="float32 深度")
        self.firmware_var = tk.StringVar()
        self.command_var = tk.StringVar()
        self.port_var = tk.StringVar()
        self._build()
        self._update_firmware_hint()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.after(33, self._poll)
        self.root.after(250, self._redraw)

    @staticmethod
    def _suggest_folder():
        return "capture_" + datetime.now().strftime("%Y%m%d_%H%M%S")

    def _label(self, parent, text="", **kwargs):
        options = dict(bg=parent.cget("bg"), fg=TEXT, font=("Segoe UI", 10))
        options.update(kwargs)
        return tk.Label(parent, text=text, **options)

    def _panel(self, parent, title):
        frame = tk.Frame(parent, bg=PANEL, padx=18, pady=16)
        self._label(frame, title, font=("Segoe UI", 14, "bold")).pack(anchor="w", pady=(0, 12))
        return frame

    def _entry(self, parent, variable, width=18):
        return tk.Entry(parent, textvariable=variable, width=width,
                        bg=PANEL_ALT, fg=TEXT, insertbackground=TEXT,
                        relief="flat", font=("Segoe UI", 10),
                        highlightthickness=1, highlightbackground=GRID,
                        highlightcolor=TEAL)

    def _button(self, parent, text, command, color=TEAL, fg=BG):
        return tk.Button(parent, text=text, command=command, bg=color,
                         fg=fg, activebackground=color, activeforeground=fg,
                         relief="flat", padx=14, pady=7,
                         font=("Segoe UI", 10, "bold"), cursor="hand2")

    def _build(self):
        header = tk.Frame(self.root, bg=BG, padx=24, pady=16)
        header.pack(fill="x")
        tk.Label(header, text="POWER DECK", bg=BG, fg=TEXT,
                 font=("Segoe UI", 22, "bold")).pack(side="left")
        self._label(header, "深度图 · 功耗实时监测", bg=BG,
                    fg=MUTED).pack(side="left", padx=(16, 0), pady=(8, 0))
        self._label(header, textvariable=self.status_var, bg=BG,
                    fg=TEAL).pack(side="right", pady=(8, 0))

        connect = tk.Frame(self.root, bg=PANEL, padx=16, pady=12)
        connect.pack(fill="x", padx=24, pady=(0, 14))
        self._label(connect, "C6 IP（留空自动发现）", fg=MUTED).pack(side="left")
        self._entry(connect, self.board_var, 22).pack(side="left", padx=(10, 12), ipady=6)
        self.connect_button = self._button(connect, "连接", self.toggle_connection)
        self.connect_button.pack(side="left")
        self._label(connect, "UDP 5005  ·  发现端口 5006  ·  自动发现时请留空 IP",
                    fg=MUTED).pack(side="left", padx=18)

        content = tk.Frame(self.root, bg=BG)
        content.pack(fill="both", expand=True, padx=24)
        content.grid_columnconfigure(0, weight=3)
        content.grid_columnconfigure(1, weight=2)
        content.grid_rowconfigure(0, weight=1)
        left = self._panel(content, "功耗曲线  /  最近 60 秒")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        self.chart = PowerChart(left)
        self.chart.pack(fill="both", expand=True)
        metrics = tk.Frame(left, bg=PANEL)
        metrics.pack(fill="x", pady=(14, 0))
        for col, (title, var, color) in enumerate((("电压", self.voltage_var, TEAL),
                                                    ("电流", self.current_var, BLUE),
                                                    ("功率", self.power_var, AMBER))):
            card = tk.Frame(metrics, bg=PANEL_ALT, padx=12, pady=8)
            card.pack(side="left", fill="x", expand=True,
                      padx=(0, 8) if col < 2 else (0, 0))
            self._label(card, title, fg=MUTED).pack(anchor="w")
            self._label(card, textvariable=var, fg=color,
                        font=("Consolas", 16, "bold")).pack(anchor="w")
        right = self._panel(content, "深度视频  /  48 × 48")
        right.grid(row=0, column=1, sticky="nsew", padx=(7, 0))
        self.video = tk.Label(right, text="等待深度图像", bg="#0f1929", fg=MUTED,
                              font=("Segoe UI", 15), width=36, height=8)
        self.video.pack(fill="both", expand=True)
        self.video_image = None
        self._label(right, textvariable=self.frame_var, fg=MUTED,
                    wraplength=420, justify="left").pack(anchor="w", pady=(12, 0))

        bottom = tk.Frame(self.root, bg=BG)
        bottom.pack(fill="x", padx=24, pady=(14, 10))
        bottom.grid_columnconfigure(0, weight=3)
        bottom.grid_columnconfigure(1, weight=2)
        recording = self._panel(bottom, "数据记录")
        recording.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        row = tk.Frame(recording, bg=PANEL)
        row.pack(fill="x")
        self._label(row, "保存位置", fg=MUTED).pack(side="left")
        self._entry(row, self.save_root_var, 41).pack(side="left", padx=9, fill="x", expand=True, ipady=5)
        self._button(row, "浏览…", self.browse_folder, PANEL_ALT, TEXT).pack(side="left")
        row2 = tk.Frame(recording, bg=PANEL)
        row2.pack(fill="x", pady=(12, 0))
        self._label(row2, "文件夹名", fg=MUTED).pack(side="left")
        self._entry(row2, self.folder_var, 25).pack(side="left", padx=9, ipady=5)
        self.record_button = self._button(row2, "开始记录", self.toggle_recording)
        self.record_button.pack(side="left", padx=(8, 0))
        self._label(recording, textvariable=self.record_var, fg=MUTED,
                    wraplength=650, justify="left").pack(anchor="w", pady=(10, 0))

        future = tk.Frame(bottom, bg=BG)
        future.grid(row=0, column=1, sticky="nsew", padx=(7, 0))
        firmware = self._panel(future, "帧类型 / 重新烧录   ·   前端预留")
        firmware.pack(fill="x")
        mode_row = tk.Frame(firmware, bg=PANEL)
        mode_row.pack(fill="x")
        selector = ttk.Combobox(mode_row, textvariable=self.format_var,
                                values=list(FORMATS), state="readonly", width=17)
        selector.pack(side="left")
        selector.bind("<<ComboboxSelected>>", lambda _event: self._update_firmware_hint())
        self._entry(mode_row, self.port_var, 10).pack(side="left", padx=8, ipady=5)
        self._label(mode_row, "COM 口", fg=MUTED).pack(side="left")
        tk.Button(mode_row, text="重新烧录", state="disabled", padx=9).pack(side="right")
        self._label(firmware, textvariable=self.firmware_var, fg=MUTED,
                    wraplength=420, justify="left").pack(anchor="w", pady=(8, 0))
        commands = self._panel(future, "WiFi 指令   ·   前端预留")
        commands.pack(fill="x", pady=(10, 0))
        cmd_row = tk.Frame(commands, bg=PANEL)
        cmd_row.pack(fill="x")
        self._entry(cmd_row, self.command_var, 32).pack(side="left", fill="x", expand=True, ipady=5)
        tk.Button(cmd_row, text="发送指令", state="disabled", padx=9).pack(side="left", padx=(8, 0))
        self._label(commands, "指令协议尚未接入；此处不会向设备发送数据。",
                    fg=MUTED).pack(anchor="w", pady=(8, 0))

        footer = tk.Frame(self.root, bg=BG, padx=24, pady=9)
        footer.pack(fill="x")
        self._label(footer, textvariable=self.stats_var, bg=BG, fg=MUTED,
                    font=("Consolas", 10)).pack(side="left")
        self._label(footer, "DPT1 / PDK1", bg=BG, fg=MUTED).pack(side="right")

    def _update_firmware_hint(self):
        number, folder = FORMATS[self.format_var.get()]
        self.firmware_var.set(f"已选择格式 {number} · bench_artifacts/{folder}/\n仅更新界面选择，尚未烧录设备。")

    def browse_folder(self):
        selected = filedialog.askdirectory(parent=self.root, title="选择记录保存位置",
                                           initialdir=self.save_root_var.get())
        if selected:
            self.save_root_var.set(selected)

    def _get_recorder(self):
        return self.recorder

    def toggle_connection(self):
        if self.receiver is not None:
            self.receiver.stop()
            self.receiver = None
            self.connect_button.configure(text="连接")
            self.status_var.set("已断开")
            return
        board = self.board_var.get().strip()
        if board:
            try:
                address = ipaddress.ip_address(board)
                if address.version != 4:
                    raise ValueError("仅支持 IPv4")
            except ValueError:
                messagebox.showerror("C6 IP 无效", "请输入 C6 的 IPv4 地址，或留空自动发现。")
                return
        self.receiver_error = False
        self.receiver = UdpReceiver(board, self.events, self._get_recorder)
        self.connect_button.configure(text="断开")
        self.status_var.set(f"正在连接 {board}…")

    def toggle_recording(self):
        if self.recorder is not None:
            current = self.recorder
            self.recorder = None
            current.stop()
            self.finishing.append(current)
            self.record_button.configure(text="开始记录", bg=TEAL)
            self.record_var.set(f"正在完成写盘：{current.folder}")
            self.folder_var.set(self._suggest_folder())
            return
        name = self.folder_var.get().strip()
        if (not name or name in (".", "..") or name.endswith((" ", "."))
                or any(char in name for char in '\\/:*?"<>|')):
            messagebox.showerror("文件夹名无效", "请输入单个文件夹名称，不要包含路径分隔符或特殊字符。")
            return
        base = Path(self.save_root_var.get()).expanduser()
        target = base / name
        try:
            target.mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            messagebox.showerror("文件夹已存在", "请更换文件夹名称，以免覆盖已有记录。")
            return
        except OSError as exc:
            messagebox.showerror("无法创建记录文件夹", str(exc))
            return
        self.recorder = CaptureRecorder(target)
        self.record_button.configure(text="停止记录", bg=RED)
        self.record_var.set(f"正在记录到：{target}")

    def _poll(self):
        newest = None
        for _ in range(256):
            try:
                kind, payload = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "power":
                stamp, _, sample = payload
                self.last_packet_at = stamp
                self.chart.add(stamp, sample)
                self.voltage_var.set(f"{sample['voltage_v']:.3f} V")
                self.current_var.set(f"{sample['current_a']:.3f} A")
                self.power_var.set(f"{sample['power_w']:.3f} W")
            elif kind == "frame":
                newest = payload
                self.last_packet_at = payload[0]
            elif kind == "stats":
                s = payload
                self.stats_var.set(
                    f"\u6df1\u5ea6 {s['fps']:.1f} fps  \u5b8c\u6574\u5e27 {s['depth']}  \u529f\u8017 {s['power']}  "
                    f"\u65e0\u6548 {s['invalid']}  \u672a\u5b8c\u6210 {s['incomplete']}  "
                    f"\u5e8f\u53f7\u7f3a\u53e3 {s['seq_lost']} / \u4f30\u7b97\u4e22\u5e27 {s['seq_loss_pct']:.2f}% "
                    f"(\u4e71\u5e8f {s['seq_reordered']})  \u754c\u9762\u8df3\u8fc7 {s['ui_dropped']}")
            elif kind == "listening":
                self.status_var.set(f"监听 {payload} · 正在发现设备")
            elif kind == "discovered":
                self.board_var.set(payload)
                self.status_var.set(f"已自动发现 {payload} · 正在接收")
            elif kind == "error":
                self.status_var.set(payload)
                messagebox.showerror("连接错误", payload)
                self.receiver = None
                self.connect_button.configure(text="连接")
            elif kind == "stopped" and self.receiver is None and not self.receiver_error:
                self.status_var.set("已断开")
        if newest:
            stamp, source, frame = newest
            self.latest_frame = frame
            name = {1: "u16+状态", 2: "float32 深度", 3: "原始字节"}.get(frame["format"], "未知")
            self.frame_var.set(
                f"{source}  ·  {name}  ·  帧 {frame['seq']}  ·  {len(frame['raw'])} B\n"
                f"C6 驻留 {frame['c6_queue_ms']} ms  ·  电脑重组 {frame['reassembly_ms']:.1f} ms")
            if frame["pixels"] is None:
                self.video.configure(image="", text="原始字节帧\n无已定义像素布局", fg=MUTED)
                self.video_image = None
            else:
                ppm = depth_ppm(frame)
                scale = max(1, min((self.video.winfo_width() - 16) // 48, (self.video.winfo_height() - 24) // 48))
                self.video_image = tk.PhotoImage(data=ppm, format="PPM").zoom(scale, scale)
                self.video.configure(image=self.video_image, text="")
            self.status_var.set(f"正在接收 {source}")
        if self.receiver and self.last_packet_at and time.monotonic()-self.last_packet_at > 3:
            self.status_var.set("已连接 · 等待数据")
        if self.recorder:
            r = self.recorder
            self.record_var.set(
                f"正在记录：{r.folder}  ·  深度 {r.frames} 帧  ·  功耗 {r.power} 条  ·  写盘队列丢弃 {r.dropped}")
            if r.error:
                r.stop()
                self.recorder = None
                self.finishing.append(r)
                self.record_button.configure(text="开始记录", bg=TEAL)
                messagebox.showerror("记录失败", r.error)
        for finished in list(self.finishing):
            if finished.closed.is_set():
                self.finishing.remove(finished)
                self.record_var.set(
                    f"记录完成：{finished.folder}  ·  深度 {finished.frames} 帧  ·  功耗 {finished.power} 条"
                    + (f"  ·  写盘失败：{finished.error}" if finished.error else ""))
        self.root.after(33, self._poll)

    def _redraw(self):
        self.chart.redraw()
        self.root.after(250, self._redraw)

    def close(self):
        if self.receiver:
            self.receiver.stop()
        if self.recorder:
            self.recorder.stop()
            self.finishing.append(self.recorder)
        for recorder in self.finishing:
            recorder.thread.join()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", default="", help="C6 IPv4 address; omit to discover on the LAN")
    args = parser.parse_args()
    root = tk.Tk()
    DeckApp(root, args.board)
    root.mainloop()


if __name__ == "__main__":
    main()
