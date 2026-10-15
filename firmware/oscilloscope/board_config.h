// ============================================================================
//  board_config.h — everything that depends on the PCB lives in this file.
//
//  If a value here does not match the KiCad schematic, change it here and
//  nowhere else. Items tagged "VERIFY" are the ones most likely to differ
//  from your board; use the PROBE serial command to check each one live.
//
//  Check the pin map BEFORE flashing the assembled PCB: the firmware drives
//  PIN_ADS_CS, PIN_SPI_SCK, PIN_SPI_MOSI (and PIN_ADS_RST if set) as outputs,
//  and an output wired to another chip's output can damage both.
// ============================================================================
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
//  XIAO ESP32 pin map. Uses the Dx/Ax silkscreen labels so the same file works
//  on the XIAO ESP32C3, ESP32S3 and ESP32C6. Set a pin to -1 if that net is not
//  routed to the XIAO.
// ---------------------------------------------------------------------------

// V2 (direct): level-shifted input, straight into an ESP32 ADC pin.      VERIFY
constexpr int PIN_ADC_DIRECT = A0;
// V1 (auto-range): CD4052 common (X) output after the per-band gain stage. VERIFY
constexpr int PIN_ADC_AUTO = A1;

// CD4052 select lines A and B. They are driven by the comparator window logic
// (MCP6544 -> SN74LVC2G08); the XIAO only *reads* them to know which gain
// band each sample came from. Must be 3.3 V logic at the XIAO pin.        VERIFY
constexpr int PIN_BAND_A = D2;
constexpr int PIN_BAND_B = D3;

// LPV7215 comparator output, usable as a hardware trigger source.        VERIFY
constexpr int PIN_HW_TRIGGER = D5;

// ADS8681 16-bit SAR ADC (V3) on the XIAO's hardware SPI pins.
constexpr int PIN_ADS_CS = D7;     // CONVST/CS                             VERIFY
// RST (active low). Optional: the ADS8681 resets itself at power-up. Set this
// to the XIAO pin (e.g. D4) only if RST is routed to the XIAO.            VERIFY
constexpr int PIN_ADS_RST = -1;
constexpr int PIN_SPI_SCK = D8;
constexpr int PIN_SPI_MISO = D9;   // ADS8681 SDO-0
constexpr int PIN_SPI_MOSI = D10;  // ADS8681 SDI

// ---------------------------------------------------------------------------
//  Analog front end (defaults; all of these can be re-calibrated at runtime
//  with the CAL / GAIN / MID commands and stored in flash with SAVE).
// ---------------------------------------------------------------------------

// Mid-rail reference: R1 = R2 = 10k divider from 3.3 V, buffered by a TSV794
// section. The level shifter (R3 = R6, R4 = R5 = 10k) is a gain-of-2
// non-inverting summing amp, so  V_ls = V_in + V_mid  and  V_in = V_ls - V_mid.
constexpr float DEFAULT_MID_V = 1.65f;

// Overall gain from the probe tip to the level-shifted node (1.0 with the
// matched 10k resistors above; change if you add an input attenuator).
constexpr float DEFAULT_FRONTEND_GAIN = 1.0f;

// Gain of the auto-range path for each CD4052 select code (B << 1 | A).
// The gain stages amplify around mid-rail (R24/R27 = 1.65 V reference), so
//   V_adc = V_mid + gain * (V_ls - V_mid).
// Window 1 (R19/R20/R21 = 5k/2k/5k) spans 1.375 V .. 1.925 V at the level-
// shifted node, i.e. +/-0.275 V at the input, so a gain of 6 maps it onto the
// full 0..3.3 V ADC range. Set these to the gains your resistors give.  VERIFY
constexpr float DEFAULT_BAND_GAIN[4] = {
    1.0f,  // code 0: outside every window -> unity-gain path
    2.0f,  // code 1
    2.0f,  // code 2
    6.0f,  // code 3: inside the innermost window -> highest gain
};

// Native ESP32 ADC attenuation. 11 dB gives the widest input range
// (about 0..3.1 V on the S3/C6, 0..2.5 V specified on the C3).
constexpr adc_attenuation_t NATIVE_ADC_ATTEN = ADC_11db;

// ---------------------------------------------------------------------------
//  ADS8681
// ---------------------------------------------------------------------------

// SCLK frequency. The ADS8681 accepts up to 66.7 MHz; 20 MHz keeps margin
// for the ESP32 GPIO-matrix input delay and hand-routed traces.
constexpr uint32_t ADS_SPI_HZ = 20000000;

// Input range code written to RANGE_SEL (see ads8681.h). The level-shifted
// signal is 0..3.3 V, so 0..5.12 V (0x0B) is the tightest range that fits.
// If you wire the probe straight to AIN_P instead, pick a bipolar range and
// set the EXT mid-point to 0 V (MID EXT 0).
constexpr uint8_t DEFAULT_ADS_RANGE = 0x0B;

// ---------------------------------------------------------------------------
//  Acquisition limits
// ---------------------------------------------------------------------------
constexpr uint32_t MAX_SAMPLES = 4096;   // record length per channel
constexpr uint32_t MIN_SAMPLES = 16;
constexpr uint32_t MAX_SAMPLE_RATE = 1000000;  // ADS8681 maximum (1 MSPS)
constexpr uint32_t MIN_SAMPLE_RATE = 1;
