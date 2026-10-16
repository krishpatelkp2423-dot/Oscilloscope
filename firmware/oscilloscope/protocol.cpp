#include "protocol.h"

namespace proto {

uint16_t crc16(uint16_t crc, const uint8_t* data, size_t len) {
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

PacketWriter::PacketWriter(Print& out, uint8_t type, uint32_t payload_len) : out_(out) {
  const uint8_t sync[2] = {SYNC0, SYNC1};
  out_.write(sync, sizeof sync);
  uint8_t head[6] = {type, VERSION,
                     (uint8_t)payload_len, (uint8_t)(payload_len >> 8),
                     (uint8_t)(payload_len >> 16), (uint8_t)(payload_len >> 24)};
  write(head, sizeof head);
}

void PacketWriter::write(const void* data, size_t len) {
  crc_ = crc16(crc_, (const uint8_t*)data, len);
  out_.write((const uint8_t*)data, len);
}

void PacketWriter::finish() {
  const uint8_t tail[2] = {(uint8_t)crc_, (uint8_t)(crc_ >> 8)};
  out_.write(tail, sizeof tail);
}

void sendText(Print& out, const char* text) {
  size_t len = strlen(text);
  PacketWriter w(out, PKT_TEXT, len);
  w.write(text, len);
  w.finish();
}

}  // namespace proto
