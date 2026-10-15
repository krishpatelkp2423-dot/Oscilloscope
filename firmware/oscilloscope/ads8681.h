// ============================================================================
//  ads8681.h — from-scratch driver for the TI ADS8681
//  (16-bit, 1 MSPS SAR ADC with an integrated programmable-range front end).
//
//  SPI frame (datasheet, "Input Command Descriptions"), 32 bits on SDI:
//      [31:25] 7-bit opcode   [24:16] 9-bit register address   [15:0] data
//  Every rising edge of CONVST/CS starts a conversion. The result is shifted
//  out on SDO-0, MSB first in straight binary, during the *next* frame.
//  A frame shorter than 32 clocks is treated as a NOP, so a 16-clock frame
//  is enough to read a conversion result.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>

class ADS8681 {
 public:
  // RANGE_SEL_REG[3:0] codes. VREF is the internal 4.096 V reference.
  enum Range : uint8_t {
    RANGE_PM_3VREF = 0x0,      // +/-12.288 V
    RANGE_PM_2V5REF = 0x1,     // +/-10.24 V
    RANGE_PM_1V5REF = 0x2,     // +/-6.144 V
    RANGE_PM_1V25REF = 0x3,    // +/-5.12 V
    RANGE_PM_0V625REF = 0x4,   // +/-2.56 V
    RANGE_UNI_3VREF = 0x8,     // 0..12.288 V
    RANGE_UNI_2V5REF = 0x9,    // 0..10.24 V
    RANGE_UNI_1V5REF = 0xA,    // 0..6.144 V
    RANGE_UNI_1V25REF = 0xB,   // 0..5.12 V
  };

  // Register byte addresses (32-bit registers, accessed as 16-bit half-words).
  static constexpr uint16_t REG_DEVICE_ID = 0x00;
  static constexpr uint16_t REG_RST_PWRCTL = 0x04;
  static constexpr uint16_t REG_SDI_CTL = 0x08;
  static constexpr uint16_t REG_SDO_CTL = 0x0C;
  static constexpr uint16_t REG_DATAOUT_CTL = 0x10;
  static constexpr uint16_t REG_RANGE_SEL = 0x14;
  static constexpr uint16_t REG_ALARM = 0x20;

  static bool isValidRange(uint8_t range);
  // Human-readable range, e.g. "0..5.120 V" or "+/-12.288 V".
  static void describeRange(uint8_t range, char* out, size_t len);

  // Resets the device, configures it and checks that it answers.
  // Returns false if no ADS8681 responds (MISO floating or stuck).
  bool begin(SPIClass& spi, int cs_pin, int rst_pin, uint32_t spi_hz);
  bool present() const { return present_; }

  // Writes RANGE_SEL and reads it back. Returns false on mismatch.
  bool setRange(uint8_t range);
  uint8_t range() const { return range_; }

  void writeReg(uint16_t addr, uint16_t value);
  uint16_t readReg(uint16_t addr);

  // ---- Fast sampling path -------------------------------------------------
  // Call beginCapture() once, then convert() once per sample, then
  // endCapture(). The SPI bus stays claimed in between.
  void beginCapture();
  void endCapture();

  // Ends the previous conversion's read-out and starts a new conversion on the
  // CS rising edge. Returns the result of the conversion started by the
  // *previous* call. The caller must leave >= 1 us between calls (t_CYC).
  inline uint16_t convert() {
    gpio_set_level((gpio_num_t)cs_, 0);
    uint16_t code = spi_->transfer16(0x0000);
    gpio_set_level((gpio_num_t)cs_, 1);
    return code;
  }

  inline float codeToVolts(uint16_t code) const { return low_v_ + code * lsb_v_; }
  float lsbVolts() const { return lsb_v_; }

 private:
  uint32_t frame32(uint32_t word);
  void hardwareReset();
  void updateScale();

  SPIClass* spi_ = nullptr;
  SPISettings settings_;
  int cs_ = -1;
  int rst_ = -1;
  bool present_ = false;
  uint8_t range_ = RANGE_UNI_1V25REF;
  float low_v_ = 0.0f;
  float lsb_v_ = 5.12f / 65536.0f;
};
