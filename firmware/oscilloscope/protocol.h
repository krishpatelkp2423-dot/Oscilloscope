// ============================================================================
//  protocol.h — binary packet format sent to the host (host/scope_protocol.py
//  implements the receiving side; keep the two in sync).
//
//  Every packet, little-endian:
//    0xA5 0x5A | type u8 | version u8 | payload_len u32 | payload | crc16 u16
//  The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type, version,
//  payload_len and payload — the same as Python's binascii.crc_hqx(.., 0xFFFF).
//
//  PKT_WAVEFORM payload: WaveformHeader, then nch float32 arrays of nsamples
//  volts (input-referred), then nsamples uint8 band codes if FLAG_HAS_BANDS.
//  PKT_TEXT payload: UTF-8 text (command replies, log messages).
// ============================================================================
#pragma once
#include <Arduino.h>

namespace proto {

constexpr uint8_t SYNC0 = 0xA5;
constexpr uint8_t SYNC1 = 0x5A;
constexpr uint8_t VERSION = 1;

enum PacketType : uint8_t {
  PKT_WAVEFORM = 1,
  PKT_TEXT = 2,
};

enum WaveformFlags : uint8_t {
  FLAG_TRIGGERED = 1 << 0,  // trig_index marks a real trigger event
  FLAG_HAS_BANDS = 1 << 1,  // a per-sample band-code array follows the data
  FLAG_SINGLE = 1 << 2,     // result of a SINGLE capture; acquisition stopped
};

struct __attribute__((packed)) WaveformHeader {
  uint32_t seq;          // increments every frame
  uint8_t mode;          // Mode enum (scope.h)
  uint8_t nch;           // number of channel arrays that follow
  uint8_t flags;         // WaveformFlags
  uint8_t trig_src;      // channel id, or TRIG_SRC_HW
  uint32_t nsamples;     // samples per channel
  int32_t trig_index;    // sample index of the trigger, -1 if none
  float sample_rate;     // measured sample rate in Hz
  float trig_level;      // trigger level in volts
  uint8_t ch_id[4];      // channel id of each array (unused slots = 0xFF)
};
static_assert(sizeof(WaveformHeader) == 28, "WaveformHeader layout changed");

uint16_t crc16(uint16_t crc, const uint8_t* data, size_t len);

// Streams one packet without building it in RAM first.
class PacketWriter {
 public:
  PacketWriter(Print& out, uint8_t type, uint32_t payload_len);
  void write(const void* data, size_t len);
  void finish();

 private:
  Print& out_;
  uint16_t crc_ = 0xFFFF;
};

void sendText(Print& out, const char* text);

}  // namespace proto
