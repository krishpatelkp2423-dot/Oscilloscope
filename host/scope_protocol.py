"""Host side of the oscilloscope's USB serial protocol.

Mirrors firmware/oscilloscope/protocol.h. Every packet, little-endian:

    A5 5A | type u8 | version u8 | payload_len u32 | payload | crc16 u16

The CRC is CRC-16/CCITT-FALSE over type, version, payload_len and payload,
which is exactly ``binascii.crc_hqx(data, 0xFFFF)``.
"""
from __future__ import annotations

import binascii
import queue
import struct
import threading
from dataclasses import dataclass

import numpy as np

SYNC = b"\xA5\x5A"
VERSION = 1
PKT_WAVEFORM = 1
PKT_TEXT = 2

FLAG_TRIGGERED = 1 << 0
FLAG_HAS_BANDS = 1 << 1
FLAG_SINGLE = 1 << 2

# seq, mode, nch, flags, trig_src, nsamples, trig_index, sample_rate,
# trig_level, ch_id[4]
WAVEFORM_HEADER = struct.Struct("<IBBBBIiff4B")
MAX_PAYLOAD = 1 << 20

MODES = ["AUTO", "DIRECT", "EXT", "ALL"]
CHANNELS = ["AUTO", "DIRECT", "EXT"]  # index = channel id
TRIGGER_SOURCES = ["AUTO", "DIRECT", "EXT", "HW"]

# Espressif's native-USB VID (XIAO ESP32C3/S3/C6), then common USB-UART bridges.
KNOWN_VIDS = (0x303A, 0x10C4, 0x1A86)


@dataclass
class Waveform:
    seq: int
    mode: int
    flags: int
    trig_src: int
    trig_index: int
    sample_rate: float
    trig_level: float
    channels: dict[str, np.ndarray]  # name -> input-referred volts
    bands: np.ndarray | None  # CD4052 select code per sample (AUTO path)

    @property
    def n(self) -> int:
        return len(next(iter(self.channels.values()))) if self.channels else 0

    @property
    def triggered(self) -> bool:
        return bool(self.flags & FLAG_TRIGGERED)

    @property
    def single(self) -> bool:
        return bool(self.flags & FLAG_SINGLE)

    def time_axis(self) -> np.ndarray:
        """Sample times in seconds; 0 is the trigger point (or the first sample)."""
        t0 = self.trig_index if self.trig_index >= 0 else 0
        return (np.arange(self.n) - t0) / self.sample_rate


def encode_packet(ptype: int, payload: bytes) -> bytes:
    body = struct.pack("<BBI", ptype, VERSION, len(payload)) + payload
    return SYNC + body + struct.pack("<H", binascii.crc_hqx(body, 0xFFFF))


def encode_waveform(
    channels: dict[str, np.ndarray],
    sample_rate: float,
    *,
    seq: int = 0,
    mode: int = 3,
    trig_index: int = -1,
    trig_src: int = 1,
    trig_level: float = 0.0,
    bands: np.ndarray | None = None,
    single: bool = False,
) -> bytes:
    """Builds a waveform packet exactly as the firmware would (tests and demo mode)."""
    ids = [CHANNELS.index(name) for name in channels]
    n = len(next(iter(channels.values())))
    flags = (FLAG_TRIGGERED if trig_index >= 0 else 0) | (FLAG_SINGLE if single else 0)
    if bands is not None:
        flags |= FLAG_HAS_BANDS
    header = WAVEFORM_HEADER.pack(
        seq, mode, len(ids), flags, trig_src, n, trig_index, sample_rate, trig_level,
        *(ids + [0xFF] * (4 - len(ids))),
    )
    payload = header + b"".join(np.asarray(v, "<f4").tobytes() for v in channels.values())
    if bands is not None:
        payload += np.asarray(bands, np.uint8).tobytes()
    return encode_packet(PKT_WAVEFORM, payload)


def decode_waveform(payload: bytes) -> Waveform:
    (seq, mode, nch, flags, trig_src, n, trig_index, rate, level, *ids) = (
        WAVEFORM_HEADER.unpack_from(payload)
    )
    expected = WAVEFORM_HEADER.size + nch * n * 4 + (n if flags & FLAG_HAS_BANDS else 0)
    if nch > len(CHANNELS) or len(payload) != expected:
        raise ValueError(f"waveform payload is {len(payload)} bytes, expected {expected}")
    offset = WAVEFORM_HEADER.size
    channels = {}
    for ch_id in ids[:nch]:
        channels[CHANNELS[ch_id]] = np.frombuffer(payload, "<f4", n, offset).astype(np.float64)
        offset += n * 4
    bands = np.frombuffer(payload, np.uint8, n, offset).copy() if flags & FLAG_HAS_BANDS else None
    return Waveform(seq, mode, flags, trig_src, trig_index, rate, level, channels, bands)


class PacketParser:
    """Incremental parser: feed() raw serial bytes, get complete packets back.

    Bytes outside a valid packet (boot messages, CSV output from before
    FORMAT BIN took effect, line noise) are skipped, and a bad CRC makes the
    parser resynchronise on the next sync word.
    """

    def __init__(self) -> None:
        self._buf = bytearray()
        self.crc_errors = 0

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self._buf += data
        packets = []
        buf = self._buf
        while True:
            start = buf.find(SYNC)
            if start < 0:
                # Keep a trailing 0xA5: it may be the first half of a sync word.
                del buf[: len(buf) - 1 if buf.endswith(SYNC[:1]) else len(buf)]
                break
            del buf[:start]
            if len(buf) < 8:
                break
            ptype, version, length = struct.unpack_from("<BBI", buf, 2)
            if version != VERSION or ptype not in (PKT_WAVEFORM, PKT_TEXT) or length > MAX_PAYLOAD:
                del buf[:1]
                continue
            total = 8 + length + 2
            if len(buf) < total:
                break
            (crc,) = struct.unpack_from("<H", buf, 8 + length)
            if binascii.crc_hqx(bytes(buf[2 : 8 + length]), 0xFFFF) != crc:
                self.crc_errors += 1
                del buf[:1]
                continue
            packets.append((ptype, bytes(buf[8 : 8 + length])))
            del buf[:total]
        return packets


def measure(y: np.ndarray, sample_rate: float) -> dict[str, float]:
    """Basic scope measurements: min, max, peak-to-peak, mean, RMS, frequency."""
    y = np.asarray(y, dtype=np.float64)
    if y.size == 0:
        return {}
    vmin, vmax = float(y.min()), float(y.max())
    result = {
        "min": vmin,
        "max": vmax,
        "pp": vmax - vmin,
        "mean": float(y.mean()),
        "rms": float(np.sqrt(np.mean(y * y))),
        "freq": float("nan"),
    }
    # Frequency from rising crossings of the midpoint, with 10 % hysteresis so
    # noise near the midpoint doesn't count as extra edges.
    mid, hyst = (vmax + vmin) / 2, 0.1 * (vmax - vmin)
    if hyst <= 0:
        return result
    crossings = []
    armed = False
    for i in range(1, y.size):
        if y[i] < mid - hyst:
            armed = True
        elif armed and y[i - 1] < mid <= y[i]:
            # linear interpolation for a sub-sample crossing time
            crossings.append(i - 1 + (mid - y[i - 1]) / (y[i] - y[i - 1]))
            armed = False
    if len(crossings) >= 2:
        period = (crossings[-1] - crossings[0]) / (len(crossings) - 1) / sample_rate
        result["freq"] = 1.0 / period
    return result


def find_port() -> str | None:
    """Returns the first serial port that looks like a XIAO ESP32."""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    for vid in KNOWN_VIDS:
        for p in ports:
            if p.vid == vid:
                return p.device
    return None


class ScopeLink:
    """Serial connection to the scope with a background reader thread."""

    def __init__(self, port: str, baud: int = 921600) -> None:
        import serial

        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.parser = PacketParser()
        self.waveforms: queue.Queue[Waveform] = queue.Queue(maxsize=8)
        self.messages: queue.Queue[str] = queue.Queue()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def send(self, command: str) -> None:
        self.ser.write(command.strip().encode("ascii") + b"\n")

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self.ser.close()

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception as exc:  # unplugged, etc.
                self.messages.put(f"serial error: {exc}")
                return
            for ptype, payload in self.parser.feed(data):
                if ptype == PKT_TEXT:
                    self.messages.put(payload.decode("utf-8", "replace"))
                    continue
                try:
                    wf = decode_waveform(payload)
                except ValueError as exc:
                    self.messages.put(f"bad waveform: {exc}")
                    continue
                if self.waveforms.full():  # GUI is behind: drop the oldest frame
                    try:
                        self.waveforms.get_nowait()
                    except queue.Empty:
                        pass
                self.waveforms.put(wf)
