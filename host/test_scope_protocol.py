"""Tests for the host protocol code. Run with:  python -m pytest host"""
import binascii

import numpy as np
import pytest

from scope_protocol import (
    PKT_TEXT,
    PKT_WAVEFORM,
    PacketParser,
    decode_waveform,
    encode_packet,
    encode_waveform,
    measure,
)


def test_crc_matches_firmware_definition():
    # CRC-16/CCITT-FALSE check value, as implemented by proto::crc16().
    assert binascii.crc_hqx(b"123456789", 0xFFFF) == 0x29B1


def test_waveform_round_trip():
    fs = 20000.0
    t = np.arange(500) / fs
    chans = {"AUTO": np.sin(2 * np.pi * 1e3 * t), "EXT": np.cos(2 * np.pi * 1e3 * t)}
    bands = (np.arange(500) % 4).astype(np.uint8)
    packet = encode_waveform(chans, fs, seq=7, trig_index=125, trig_level=0.1, bands=bands)

    [(ptype, payload)] = PacketParser().feed(packet)
    wf = decode_waveform(payload)
    assert ptype == PKT_WAVEFORM
    assert (wf.seq, wf.trig_index, wf.n, wf.triggered) == (7, 125, 500, True)
    assert wf.sample_rate == fs
    assert list(wf.channels) == ["AUTO", "EXT"]
    np.testing.assert_allclose(wf.channels["EXT"], chans["EXT"], atol=1e-6)
    np.testing.assert_array_equal(wf.bands, bands)
    assert wf.time_axis()[125] == 0.0


def test_parser_resyncs_through_noise_split_reads_and_bad_crc():
    good = encode_packet(PKT_TEXT, b"hello")
    corrupt = bytearray(encode_packet(PKT_TEXT, b"bad!"))
    corrupt[9] ^= 0xFF
    stream = b"boot text\r\n\xa5" + bytes(corrupt) + good + b"auto:0.1\r\n" + good
    parser = PacketParser()
    packets = []
    for i in range(0, len(stream), 3):  # arrive in tiny chunks
        packets += parser.feed(stream[i:i + 3])
    assert packets == [(PKT_TEXT, b"hello"), (PKT_TEXT, b"hello")]
    assert parser.crc_errors == 1


def test_decode_rejects_truncated_payload():
    packet = encode_waveform({"DIRECT": np.zeros(100)}, 1000.0)
    [(_, payload)] = PacketParser().feed(packet)
    with pytest.raises(ValueError):
        decode_waveform(payload[:-4])


def test_measure_sine():
    fs, f = 50000.0, 1234.0
    t = np.arange(4000) / fs
    m = measure(0.5 + 1.0 * np.sin(2 * np.pi * f * t), fs)
    assert m["pp"] == pytest.approx(2.0, abs=1e-3)
    assert m["mean"] == pytest.approx(0.5, abs=5e-3)
    assert m["freq"] == pytest.approx(f, rel=1e-3)


def test_measure_dc_has_no_frequency():
    m = measure(np.full(100, 0.25), 1000.0)
    assert np.isnan(m["freq"]) and m["pp"] == 0.0
