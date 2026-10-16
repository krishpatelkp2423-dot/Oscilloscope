#!/usr/bin/env python3
"""Real-time display and control panel for the XIAO ESP32 oscilloscope.

    python scope_gui.py               # auto-detect the XIAO's USB serial port
    python scope_gui.py --port COM5   # or /dev/ttyACM0, /dev/cu.usbmodem1101
    python scope_gui.py --demo        # no hardware: simulated signal
"""
from __future__ import annotations

import argparse
import csv
import queue
import sys
import threading
import time

import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

from scope_protocol import (
    CHANNELS,
    PKT_WAVEFORM,
    PacketParser,
    ScopeLink,
    Waveform,
    decode_waveform,
    encode_waveform,
    find_port,
    measure,
)

COLORS = {"AUTO": "#f6c90e", "DIRECT": "#3ec1ff", "EXT": "#ff5fd2"}
LABELS = {"AUTO": "V1 auto-range", "DIRECT": "V2 direct ADC", "EXT": "V3 ADS8681"}
RATES = [1_000, 2_000, 5_000, 10_000, 20_000, 50_000, 100_000, 200_000, 500_000]
LENGTHS = [250, 500, 1000, 2000, 4096]
MEASUREMENTS = [("min", "Min", "V"), ("max", "Max", "V"), ("pp", "Pk-pk", "V"),
                ("mean", "Mean", "V"), ("rms", "RMS", "V"), ("freq", "Freq", "Hz")]


class DemoLink:
    """Stands in for ScopeLink without hardware. Frames are encoded and parsed
    with the real protocol code, so the demo exercises the same data path."""

    BAND_GAIN = {0: 1.0, 1: 5.5, 2: 2.36}  # 0: direct, 1: stage 1, 2: stage 2

    def __init__(self) -> None:
        self.waveforms: queue.Queue[Waveform] = queue.Queue(maxsize=8)
        self.messages: queue.Queue[str] = queue.Queue()
        self.cfg = {"MODE": "ALL", "RATE": 20000, "N": 1000, "PRE": 25, "LEVEL": 0.0,
                    "TRIG": "AUTO", "EDGE": "RISING"}
        self.running = False
        self.single = False
        self._seq = 0
        self._parser = PacketParser()
        self._rng = np.random.default_rng(1)
        self._stop = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()
        self.messages.put("Demo mode: simulated 1 kHz signal, no hardware connected")

    def send(self, command: str) -> None:
        parts = command.split()
        key = parts[0].upper()
        if key == "RUN":
            self.running, self.single = True, False
        elif key == "STOP":
            self.running = False
        elif key == "SINGLE":
            self.running, self.single = True, True
        elif key in self.cfg and len(parts) > 1:
            value = parts[1].upper()
            if key == "LEVEL":
                self.cfg[key] = float(value)
            elif key in ("RATE", "N", "PRE"):
                self.cfg[key] = int(value)
            else:
                self.cfg[key] = value
        elif key not in ("FORMAT", "SRC", "HYST"):
            self.messages.put(f"(demo) {command} has no effect without hardware")
            return
        self.messages.put(f"OK {command}")

    def close(self) -> None:
        self._stop.set()

    def _run(self) -> None:
        while not self._stop.wait(0.05):
            if self.running:
                self._emit_frame()

    def _emit_frame(self) -> None:
        cfg = self.cfg
        n, fs = cfg["N"], float(cfg["RATE"])
        pre = n * cfg["PRE"] // 100
        amp = 0.9
        level = cfg["LEVEL"]
        triggered = cfg["TRIG"] != "FREE" and abs(level) < amp
        # Phase chosen so t = 0 is a crossing of the trigger level.
        phase = np.arcsin(np.clip(level / amp, -1, 1)) if triggered else self._rng.uniform(0, 6.28)
        if cfg["EDGE"] == "FALLING":
            phase = np.pi - phase
        t = (np.arange(n) - (pre if triggered else 0)) / fs
        sig = amp * np.sin(2 * np.pi * 1000 * t + phase)
        bands = np.where(np.abs(sig) < 0.275, 1, np.where(np.abs(sig) < 0.70, 2, 0)).astype(np.uint8)
        gain = np.vectorize(self.BAND_GAIN.get)(bands)
        paths = {
            "AUTO": sig + self._rng.normal(0, 0.004, n) / gain,
            "DIRECT": np.round(sig + self._rng.normal(0, 0.004, n), 3),
            "EXT": sig + self._rng.normal(0, 0.0002, n),
        }
        keep = CHANNELS if cfg["MODE"] == "ALL" else [cfg["MODE"]]
        packet = encode_waveform(
            {k: paths[k] for k in keep}, fs, seq=self._seq,
            trig_index=pre if triggered else -1, trig_level=level,
            bands=bands if "AUTO" in keep else None, single=self.single,
        )
        self._seq += 1
        for ptype, payload in self._parser.feed(packet):
            if ptype == PKT_WAVEFORM and not self.waveforms.full():
                self.waveforms.put(decode_waveform(payload))
        if self.single:
            self.running = self.single = False


class ScopeWindow(QtWidgets.QMainWindow):
    def __init__(self, link, title: str) -> None:
        super().__init__()
        self.link = link
        self.last: Waveform | None = None
        self.frame_times: list[float] = []
        self.setWindowTitle(title)
        self.resize(1280, 760)

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        layout = QtWidgets.QHBoxLayout(central)
        layout.addWidget(self._build_plots(), stretch=1)
        layout.addWidget(self._build_controls())

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self._poll)
        self.timer.start(30)
        self._push_settings()

    # ---- layout -----------------------------------------------------------
    def _build_plots(self) -> QtWidgets.QWidget:
        plots = pg.GraphicsLayoutWidget()
        self.plot = plots.addPlot(row=0, col=0)
        self.plot.setLabel("left", "Input", units="V")
        self.plot.setLabel("bottom", "Time from trigger", units="s")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.addLegend(offset=(10, 10))
        self.plot.setYRange(-1.8, 1.8)
        self.curves = {
            name: self.plot.plot(pen=pg.mkPen(COLORS[name], width=1.5), name=LABELS[name])
            for name in CHANNELS
        }
        self.trig_line = pg.InfiniteLine(
            angle=0, movable=True, pen=pg.mkPen("#ff8c00", width=1, style=QtCore.Qt.PenStyle.DashLine),
            label="trig", labelOpts={"position": 0.03, "color": "#ff8c00"},
        )
        self.trig_line.sigPositionChangeFinished.connect(
            lambda line: self.level.setValue(round(line.value(), 3))
        )
        self.plot.addItem(self.trig_line)
        self.plot.addItem(pg.InfiniteLine(
            pos=0, angle=90, pen=pg.mkPen("#888888", style=QtCore.Qt.PenStyle.DotLine)))

        self.band_plot = plots.addPlot(row=1, col=0)
        self.band_plot.setMaximumHeight(120)
        self.band_plot.setXLink(self.plot)
        self.band_plot.setLabel("left", "V1 path")
        self.band_plot.setLabel("bottom", units="s")
        self.band_plot.setYRange(-0.3, 2.3)
        self.band_plot.getAxis("left").setTicks([[(0, "×1"), (1, "S1"), (2, "S2")]])
        self.band_curve = self.band_plot.plot(pen=pg.mkPen(COLORS["AUTO"]), stepMode="center")
        return plots

    def _build_controls(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        panel.setFixedWidth(330)
        v = QtWidgets.QVBoxLayout(panel)

        form = QtWidgets.QFormLayout()
        self.mode = self._combo(["ALL", "AUTO", "DIRECT", "EXT"], lambda s: f"MODE {s}")
        form.addRow("Acquisition", self.mode)
        self.rate = self._combo([str(r) for r in RATES], lambda s: f"RATE {s}", editable=True)
        self.rate.setCurrentText("20000")
        form.addRow("Sample rate (Hz)", self.rate)
        self.length = self._combo([str(n) for n in LENGTHS], lambda s: f"N {s}")
        self.length.setCurrentText("1000")
        form.addRow("Record length", self.length)
        self.trig = self._combo(["AUTO", "NORMAL", "FREE"], lambda s: f"TRIG {s}")
        form.addRow("Trigger mode", self.trig)
        self.edge = self._combo(["RISING", "FALLING"], lambda s: f"EDGE {s}")
        form.addRow("Edge", self.edge)
        self.src = self._combo(["DIRECT", "AUTO", "EXT"], lambda s: f"SRC {s}")
        form.addRow("Trigger source", self.src)
        self.level = QtWidgets.QDoubleSpinBox(decimals=3, minimum=-15, maximum=15, singleStep=0.05,
                                              suffix=" V", keyboardTracking=False)
        self.level.valueChanged.connect(self._level_changed)
        form.addRow("Trigger level", self.level)
        self.pre = QtWidgets.QSpinBox(minimum=0, maximum=100, value=25, suffix=" %",
                                      keyboardTracking=False)
        self.pre.valueChanged.connect(lambda val: self.link.send(f"PRE {val}"))
        form.addRow("Pre-trigger", self.pre)
        self.auto_y = QtWidgets.QCheckBox("Auto-scale vertical")
        self.auto_y.toggled.connect(self._auto_y_toggled)
        form.addRow(self.auto_y)
        v.addLayout(form)

        v.addLayout(self._button_row([("Run", "RUN"), ("Stop", "STOP"), ("Single", "SINGLE")]))
        tools = QtWidgets.QHBoxLayout()
        for text, slot in [("Save CSV", self._save_csv), ("Zero cal", self._zero_cal),
                           ("Probe", lambda: self.link.send("PROBE")),
                           ("Info", lambda: self.link.send("INFO"))]:
            button = QtWidgets.QPushButton(text)
            button.clicked.connect(slot)
            tools.addWidget(button)
        v.addLayout(tools)

        self.table = QtWidgets.QTableWidget(len(MEASUREMENTS), len(CHANNELS))
        self.table.setHorizontalHeaderLabels(["V1 auto", "V2 direct", "V3 ADS"])
        self.table.setVerticalHeaderLabels([m[1] for m in MEASUREMENTS])
        self.table.horizontalHeader().setSectionResizeMode(QtWidgets.QHeaderView.ResizeMode.Stretch)
        self.table.verticalHeader().setSectionResizeMode(QtWidgets.QHeaderView.ResizeMode.Stretch)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setFixedHeight(200)
        v.addWidget(self.table)

        self.status = QtWidgets.QLabel("waiting for data…")
        self.status.setWordWrap(True)
        v.addWidget(self.status)

        self.log = QtWidgets.QPlainTextEdit(readOnly=True)
        self.log.setMaximumBlockCount(500)
        v.addWidget(self.log, stretch=1)

        self.command = QtWidgets.QLineEdit(placeholderText="Send a command (HELP)…")
        self.command.returnPressed.connect(self._send_typed)
        v.addWidget(self.command)
        return panel

    def _combo(self, items, make_command, editable=False) -> QtWidgets.QComboBox:
        combo = QtWidgets.QComboBox(editable=editable)
        combo.addItems(items)
        if editable:
            combo.lineEdit().editingFinished.connect(
                lambda: self.link.send(make_command(combo.currentText())))
            combo.activated.connect(lambda _: self.link.send(make_command(combo.currentText())))
        else:
            combo.currentTextChanged.connect(lambda text: self.link.send(make_command(text)))
        return combo

    def _button_row(self, buttons) -> QtWidgets.QHBoxLayout:
        row = QtWidgets.QHBoxLayout()
        for text, command in buttons:
            button = QtWidgets.QPushButton(text)
            button.clicked.connect(lambda _=False, c=command: self.link.send(c))
            row.addWidget(button)
        return row

    # ---- device control -----------------------------------------------------
    def _push_settings(self) -> None:
        """The GUI is the source of truth: send every setting, then start."""
        for command in [
            "FORMAT BIN", f"MODE {self.mode.currentText()}", f"RATE {self.rate.currentText()}",
            f"N {self.length.currentText()}", f"TRIG {self.trig.currentText()}",
            f"EDGE {self.edge.currentText()}", f"SRC {self.src.currentText()}",
            f"LEVEL {self.level.value()}", f"PRE {self.pre.value()}", "RUN",
        ]:
            self.link.send(command)

    def _level_changed(self, value: float) -> None:
        self.trig_line.setValue(value)
        self.link.send(f"LEVEL {value:.4f}")

    def _auto_y_toggled(self, on: bool) -> None:
        if on:
            self.plot.enableAutoRange(axis="y")
        else:
            self.plot.setYRange(-1.8, 1.8)

    def _send_typed(self) -> None:
        text = self.command.text().strip()
        if text:
            self.log.appendPlainText(f"> {text}")
            self.link.send(text)
        self.command.clear()

    def _zero_cal(self) -> None:
        answer = QtWidgets.QMessageBox.question(
            self, "Zero calibration",
            "Short the probe input to GND, then press OK.\n\n"
            "Each path's mid-point will be measured and saved to the scope's flash.",
            QtWidgets.QMessageBox.StandardButton.Ok | QtWidgets.QMessageBox.StandardButton.Cancel,
        )
        if answer == QtWidgets.QMessageBox.StandardButton.Ok:
            self.link.send("CAL ZERO")
            self.link.send("SAVE")

    def _save_csv(self) -> None:
        wf = self.last
        if wf is None:
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save waveform", f"capture_{time.strftime('%Y%m%d_%H%M%S')}.csv", "CSV (*.csv)")
        if path:
            write_csv(path, wf)
            self.log.appendPlainText(f"saved {path}")

    # ---- display ------------------------------------------------------------
    def _poll(self) -> None:
        while True:
            try:
                self.log.appendPlainText(self.link.messages.get_nowait())
            except queue.Empty:
                break
        wf = None
        while True:  # show only the newest frame; skip any backlog
            try:
                wf = self.link.waveforms.get_nowait()
            except queue.Empty:
                break
        if wf is not None:
            self._show(wf)

    def _show(self, wf: Waveform) -> None:
        self.last = wf
        now = time.monotonic()
        self.frame_times = [t for t in self.frame_times if now - t < 2.0] + [now]

        t = wf.time_axis()
        for col, name in enumerate(CHANNELS):
            y = wf.channels.get(name)
            if y is None:
                self.curves[name].setData([], [])
                for row in range(len(MEASUREMENTS)):
                    self.table.setItem(row, col, QtWidgets.QTableWidgetItem("—"))
                continue
            self.curves[name].setData(t, y)
            m = measure(y, wf.sample_rate)
            for row, (key, _, unit) in enumerate(MEASUREMENTS):
                value = m[key]
                text = "—" if not np.isfinite(value) else pg.siFormat(value, precision=4, suffix=unit)
                self.table.setItem(row, col, QtWidgets.QTableWidgetItem(text))

        if wf.bands is not None:
            dt = 1.0 / wf.sample_rate
            self.band_curve.setData(np.append(t, t[-1] + dt) - dt / 2, wf.bands.astype(float))
        else:
            self.band_curve.setData([], [])

        fps = (len(self.frame_times) - 1) / max(now - self.frame_times[0], 1e-6)
        state = "triggered" if wf.triggered else "untriggered"
        self.status.setText(
            f"{pg.siFormat(wf.sample_rate, suffix='S/s')} measured · {wf.n} samples · "
            f"{pg.siFormat(wf.n / wf.sample_rate, suffix='s')} window · {state} · {fps:.1f} frames/s"
            + (" · single shot done" if wf.single else "")
        )

    def closeEvent(self, event) -> None:
        try:
            self.link.send("STOP")
            self.link.send("FORMAT CSV")
        finally:
            self.link.close()
        super().closeEvent(event)


def write_csv(path: str, wf: Waveform) -> None:
    names = list(wf.channels)
    with open(path, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["time_s"] + [f"{n}_V" for n in names] + (["band"] if wf.bands is not None else []))
        t = wf.time_axis()
        for i in range(wf.n):
            row = [f"{t[i]:.9g}"] + [f"{wf.channels[n][i]:.6f}" for n in names]
            if wf.bands is not None:
                row.append(int(wf.bands[i]))
            out.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--port", help="serial port (default: auto-detect)")
    parser.add_argument("--demo", action="store_true", help="run with a simulated signal")
    args = parser.parse_args()

    app = pg.mkQApp("XIAO Oscilloscope")
    if args.demo:
        link, title = DemoLink(), "XIAO Oscilloscope — demo"
    else:
        port = args.port or find_port()
        if port is None:
            print("No XIAO ESP32 found. Plug it in or pass --port (or try --demo).", file=sys.stderr)
            return 1
        link, title = ScopeLink(port), f"XIAO Oscilloscope — {port}"
    window = ScopeWindow(link, title)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
