#!/usr/bin/env python3
"""Headless capture: grab triggered frames to CSV and print measurements.

Useful for the V1 / V2 / V3 comparison, e.g. capture all three paths at once:

    python scope_capture.py --mode ALL --rate 20000 --n 2000 --frames 5 --out run.csv

Writes run_000.csv, run_001.csv, ... (one file per frame).
"""
from __future__ import annotations

import argparse
import queue
import sys
import time
from pathlib import Path

from scope_gui import write_csv
from scope_protocol import ScopeLink, find_port, measure


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--port", help="serial port (default: auto-detect)")
    p.add_argument("--mode", default="ALL", choices=["ALL", "AUTO", "DIRECT", "EXT"])
    p.add_argument("--rate", type=int, default=20000, help="sample rate in Hz")
    p.add_argument("--n", type=int, default=1000, help="samples per frame")
    p.add_argument("--trig", default="AUTO", choices=["AUTO", "NORMAL", "FREE"])
    p.add_argument("--edge", default="RISING", choices=["RISING", "FALLING"])
    p.add_argument("--src", default="DIRECT", choices=["AUTO", "DIRECT", "EXT"])
    p.add_argument("--level", type=float, default=0.0, help="trigger level in volts")
    p.add_argument("--pre", type=int, default=25, help="pre-trigger percent")
    p.add_argument("--frames", type=int, default=1)
    p.add_argument("--out", default="capture.csv", help="output CSV path (frame number appended)")
    p.add_argument("--timeout", type=float, default=10.0, help="seconds to wait per frame")
    args = p.parse_args()

    port = args.port or find_port()
    if port is None:
        print("No XIAO ESP32 found; pass --port.", file=sys.stderr)
        return 1
    link = ScopeLink(port)
    try:
        for command in ["STOP", "FORMAT BIN", f"MODE {args.mode}", f"RATE {args.rate}",
                        f"N {args.n}", f"TRIG {args.trig}", f"EDGE {args.edge}",
                        f"SRC {args.src}", f"LEVEL {args.level}", f"PRE {args.pre}", "RUN"]:
            link.send(command)
        time.sleep(0.3)
        while not link.waveforms.empty():  # discard frames captured with old settings
            link.waveforms.get_nowait()

        out = Path(args.out)
        for i in range(args.frames):
            try:
                wf = link.waveforms.get(timeout=args.timeout)
            except queue.Empty:
                print(f"no frame within {args.timeout} s (trigger never fired?)", file=sys.stderr)
                return 2
            path = out.with_name(f"{out.stem}_{i:03d}{out.suffix}") if args.frames > 1 else out
            write_csv(str(path), wf)
            print(f"{path}: {wf.n} samples at {wf.sample_rate:.0f} S/s, "
                  f"{'triggered' if wf.triggered else 'untriggered'}")
            for name, y in wf.channels.items():
                m = measure(y, wf.sample_rate)
                print(f"  {name:6s} min {m['min']:+.5f} V  max {m['max']:+.5f} V  "
                      f"pk-pk {m['pp']:.5f} V  mean {m['mean']:+.5f} V  rms {m['rms']:.5f} V  "
                      f"freq {m['freq']:.2f} Hz")
        while not link.messages.empty():
            print("device:", link.messages.get_nowait())
    finally:
        link.send("STOP")
        link.send("FORMAT CSV")
        time.sleep(0.1)
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
