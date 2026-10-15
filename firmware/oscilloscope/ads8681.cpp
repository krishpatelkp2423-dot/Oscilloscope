#include "ads8681.h"

namespace {

constexpr uint8_t OP_READ_HWORD = 0x64;  // 0b1100100
constexpr uint8_t OP_WRITE = 0x68;       // 0b1101000: write a full half-word
constexpr float VREF = 4.096f;           // internal reference

inline uint32_t command(uint8_t opcode, uint16_t addr, uint16_t data) {
  return ((uint32_t)opcode << 25) | ((uint32_t)(addr & 0x1FF) << 16) | data;
}

// Full-scale magnitude as a multiple of VREF; 0 for an invalid code.
float rangeScale(uint8_t range) {
  switch (range) {
    case 0x0: case 0x8: return 3.0f;
    case 0x1: case 0x9: return 2.5f;
    case 0x2: case 0xA: return 1.5f;
    case 0x3: case 0xB: return 1.25f;
    case 0x4: return 0.625f;
    default: return 0.0f;
  }
}

inline bool isUnipolar(uint8_t range) { return range & 0x8; }

}  // namespace

bool ADS8681::isValidRange(uint8_t range) { return rangeScale(range) > 0.0f; }

void ADS8681::describeRange(uint8_t range, char* out, size_t len) {
  float fs = rangeScale(range) * VREF;
  if (fs <= 0.0f) {
    snprintf(out, len, "invalid (0x%X)", range);
  } else if (isUnipolar(range)) {
    snprintf(out, len, "0..%.3f V", fs);
  } else {
    snprintf(out, len, "+/-%.3f V", fs);
  }
}

bool ADS8681::begin(SPIClass& spi, int cs_pin, int rst_pin, uint32_t spi_hz) {
  spi_ = &spi;
  cs_ = cs_pin;
  rst_ = rst_pin;
  settings_ = SPISettings(spi_hz, MSBFIRST, SPI_MODE0);

  pinMode(cs_, OUTPUT);
  gpio_set_level((gpio_num_t)cs_, 1);
  if (rst_ >= 0) {
    pinMode(rst_, OUTPUT);
    gpio_set_level((gpio_num_t)rst_, 1);
  }
  delay(20);  // power-up time before the first conversion
  hardwareReset();

  // Explicitly select SPI-00 (CPOL 0, CPHA 0), SDO-0 only, and conversion
  // data in the output word. These are the reset defaults; writing them makes
  // the driver independent of whatever state the chip was left in.
  writeReg(REG_SDI_CTL, 0x0000);
  writeReg(REG_SDO_CTL, 0x0000);
  writeReg(REG_DATAOUT_CTL, 0x0000);

  // Presence check: write a non-zero code and read it back. A missing chip
  // reads back 0x0000 or 0xFFFF on a floating/stuck MISO line.
  writeReg(REG_RANGE_SEL, RANGE_UNI_1V25REF);
  present_ = (readReg(REG_RANGE_SEL) & 0x004F) == RANGE_UNI_1V25REF;
  if (present_) setRange(range_);
  return present_;
}

void ADS8681::hardwareReset() {
  if (rst_ < 0) return;
  gpio_set_level((gpio_num_t)rst_, 0);
  delayMicroseconds(10);
  gpio_set_level((gpio_num_t)rst_, 1);
  delay(20);  // RST defaults to a power-on reset; wait for it to complete
}

bool ADS8681::setRange(uint8_t range) {
  if (!isValidRange(range)) return false;
  // Bit 6 (INTREF_DIS) = 0 keeps the internal 4.096 V reference enabled.
  writeReg(REG_RANGE_SEL, range);
  bool ok = (readReg(REG_RANGE_SEL) & 0x004F) == range;
  if (ok) {
    range_ = range;
    updateScale();
  }
  return ok;
}

void ADS8681::updateScale() {
  float fs = rangeScale(range_) * VREF;
  if (isUnipolar(range_)) {
    low_v_ = 0.0f;
    lsb_v_ = fs / 65536.0f;
  } else {
    low_v_ = -fs;  // straight binary: 0x0000 = -FS, 0x8000 = 0 V
    lsb_v_ = 2.0f * fs / 65536.0f;
  }
}

void ADS8681::writeReg(uint16_t addr, uint16_t value) {
  frame32(command(OP_WRITE, addr, value));
}

uint16_t ADS8681::readReg(uint16_t addr) {
  frame32(command(OP_READ_HWORD, addr, 0));
  // The register contents come out in the first 16 bits of the next frame.
  return (uint16_t)(frame32(0) >> 16);
}

uint32_t ADS8681::frame32(uint32_t word) {
  spi_->beginTransaction(settings_);
  gpio_set_level((gpio_num_t)cs_, 0);
  uint32_t rx = spi_->transfer32(word);
  gpio_set_level((gpio_num_t)cs_, 1);
  spi_->endTransaction();
  delayMicroseconds(2);  // CS rising edge started a conversion; let it finish
  return rx;
}

void ADS8681::beginCapture() {
  spi_->beginTransaction(settings_);
}

void ADS8681::endCapture() {
  spi_->endTransaction();
}
