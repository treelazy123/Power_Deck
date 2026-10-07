#!/usr/bin/env python3
"""Receive power + depth: python tool/deck_receiver.py --view --save captures."""
import argparse
import csv
import json
import math
import socket
import struct
import time
from datetime import datetime, timezone
from pathlib import Path
from .depth_protocol import Reassembler

class Viewer:
    def __init__(self,max_mm):
        import tkinter as tk
        self.tk=tk; self.root=tk.Tk(); self.root.title("Power Deck - depth / power")
        self.label=tk.Label(self.root); self.label.pack()
        self.status=tk.Label(self.root,text="Waiting for Power Deck..."); self.status.pack()
        self.max_mm=max_mm; self.closed=False
        self.root.protocol("WM_DELETE_WINDOW",self.close)
    def close(self): self.closed=True; self.root.destroy()
    def draw(self,frame):
        colors=bytearray()
        if frame["pixels"] is None: return
        for depth,valid in frame["pixels"]:
            if not valid: colors.extend((0,0,0)); continue
            t=min(1,max(0,depth/self.max_mm))
            colors.extend((int(255*(1-t)),int(255*(1-abs(2*t-1))),int(255*t)))
        ppm=f"P6\n{frame['width']} {frame['height']}\n255\n".encode()+colors
        self.image=self.tk.PhotoImage(data=ppm,format="PPM").zoom(8,8)
        self.label.configure(image=self.image)
    def update(self,text):
        if not self.closed:
            self.status.configure(text=text); self.root.update()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bind",default="0.0.0.0")
    ap.add_argument("--port",type=int,default=5005)
    ap.add_argument("--board",default="255.255.255.255",help="C6 IP, or subnet broadcast; auto discovery by default")
    ap.add_argument("--discovery-port",type=int,default=5006)
    ap.add_argument("--view",action="store_true",help="Live depth view using tkinter")
    ap.add_argument("--save",type=Path,help="Save power CSV, frame CSV and CRC-validated .bin / .pgm files")
    ap.add_argument("--max-mm",type=int,default=3000,help="View colour scale")
    ap.add_argument("--duration",type=float,default=0,help="Stop after N seconds (0: until Ctrl+C)")
    args=ap.parse_args()
    if args.max_mm<=0 or not (1<=args.port<=65535) or not (1<=args.discovery_port<=65535): ap.error("Invalid range")
    viewer=Viewer(args.max_mm) if args.view else None
    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1)
    sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024*1024)
    sock.bind((args.bind,args.port)); sock.setblocking(False)
    reassembler=Reassembler(); power_count=0; invalid=0; discovery_errors=0
    latest_power="power: waiting"; latest_depth="depth: waiting"
    files=[]; writers={}; run_path=None
    if args.save:
        run_path=args.save/datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        run_path.mkdir(parents=True)
        for name,columns in [("power",["host_utc","board","session","seq","timestamp_ms","voltage_v","current_a","power_w"]),
                             ("depth",["host_utc","board","session","seq","camera_ms","c6_rx_ms","c6_queue_ms","reassembly_ms","format","payload_size","file"])]:
            f=(run_path/(name+".csv")).open("w",newline="",encoding="utf-8"); files.append(f)
            writers[name]=csv.writer(f); writers[name].writerow(columns)
    start=time.monotonic(); next_hello=0; next_report=start+1; previous=0
    print(f"Listening UDP {args.bind}:{args.port}; discover {args.board}:{args.discovery_port}",flush=True)
    if run_path: print(f"Saving to {run_path}",flush=True)
    try:
        while not viewer or not viewer.closed:
            now=time.monotonic()
            if args.duration and now-start>=args.duration: break
            if now>=next_hello:
                try: sock.sendto(b"PDHELLO1",(args.board,args.discovery_port))
                except OSError: discovery_errors+=1
                next_hello=now+1
            newest=None
            # Bound each UI iteration; process network outside per-pixel drawing.
            for _ in range(256):
                try: raw,address=sock.recvfrom(65535)
                except BlockingIOError: break
                now=time.monotonic(); source=address[0]
                if args.board not in ("255.255.255.255",source) and not args.board.endswith(".255"):
                    continue
                utc=datetime.now(timezone.utc).isoformat()
                try:
                    if raw.startswith(b"{"):
                        obj=json.loads(raw)
                        if obj.get("type")!="power": raise ValueError("Unexpected JSON type")
                        for key in ("voltage_v","current_a","power_w"):
                            if not isinstance(obj[key],(float,int)) or not math.isfinite(obj[key]): raise ValueError("Invalid power")
                        for key in ("session","seq","timestamp_ms"):
                            if not isinstance(obj[key],int) or obj[key]<0: raise ValueError("Invalid counter")
                        power_count+=1
                        latest_power=f"U={obj['voltage_v']:.3f}V I={obj['current_a']:.3f}A P={obj['power_w']:.3f}W"
                        if writers: writers["power"].writerow([utc,source]+[obj[k] for k in ("session","seq","timestamp_ms","voltage_v","current_a","power_w")])
                    else:
                        frame=reassembler.feed(raw,address,now)
                        if frame is None: continue
                        newest=frame
                        latest_depth=f"frame={frame['seq']} format={frame['format']} bytes={len(frame['raw'])} C6-residence={frame['c6_queue_ms']}ms reassembly={frame['reassembly_ms']:.2f}ms"
                        if writers:
                            stem=f"{source}_{frame['session']:08x}_{frame['c6_rx_ms']:08x}_{frame['seq']:08x}"
                            (run_path/(stem+".bin")).write_bytes(frame["raw"])
                            if frame["pixels"] is not None:
                                pgm=b"P5\n48 48\n65535\n"+b"".join(struct.pack(">H",min(65535,d)) for d,s in frame["pixels"])
                                (run_path/(stem+".pgm")).write_bytes(pgm)
                            writers["depth"].writerow([utc,source]+[frame[k] for k in ("session","seq","camera_ms","c6_rx_ms","c6_queue_ms","reassembly_ms","format","payload_size")]+[stem+".bin"])
                except (ValueError,KeyError,TypeError,AttributeError,UnicodeError,struct.error): invalid+=1
            now=time.monotonic(); reassembler.expire(now)
            if newest and viewer and newest["pixels"] is not None: viewer.draw(newest)
            if now>=next_report:
                complete=reassembler.stats["complete"]
                elapsed=now-(next_report-1)
                print(f"depth={complete} fps={(complete-previous)/elapsed:.1f} power={power_count} invalid={invalid} "
                      f"incomplete={reassembler.stats['incomplete']} evicted={reassembler.stats['evicted']} "
                      f"pending_peak={reassembler.stats['pending_peak']} discovery_errors={discovery_errors} | {latest_power} | {latest_depth}",flush=True)
                previous=complete; next_report=now+1
                for f in files: f.flush()
            if viewer: viewer.update(latest_power+"\n"+latest_depth)
            time.sleep(0.002)
    except KeyboardInterrupt: pass
    finally:
        sock.close()
        for f in files: f.close()
        if viewer and not viewer.closed: viewer.close()
        print(f"Stopped: complete={reassembler.stats['complete']} power={power_count} invalid={invalid} incomplete={reassembler.stats['incomplete']}",flush=True)
if __name__=="__main__": main()
