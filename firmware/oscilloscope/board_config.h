// ============================================================================
//  board_config.h — everything that depends on the PCB lives in this file.
//
//  Pin map taken from the KiCad schematic and PCB layout (XIAO header U2):
//    D0  OpAmpOut1  gain stage 1 output (TSV794 section 4, fed by CD4052 Y1)
//    D1  OpAmpOut2  gain stage 2 output (TSV794 section 3, fed by CD4052 Y2)
//    D2  Input      level-shifted signal, V_in + 1.65 V (TSV794 section 2)
//    D3..D5         not connected
//    D6  RST        ADS8681 RST_N
//    D7  MIS1       ADS8681 SDI   (MOSI)
//    D8  MIS0       ADS8681 SDO-0 (MISO)
//    D9  SCLK       ADS8681 SCLK
//    D10 CS         ADS8681 CONVST/CS
//  The XIAO does not see the CD4052 select lines (AndOut1/AndOut2) or the
//  LM397/LPV7215 over-range output (MUXEN), so the firmware works out which
//  gain stage is live from the readings themselves (see scope.cpp).
//
//  Gains, offsets and windows below are design values; CAL BANDS measures the
//  real ones on your board and SAVE keeps them.
// ============================================================================
#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
//  XIAO pins (Dx labels work on the XIAO ESP32C3, ESP32S3 and ESP32C6)
// ---------------------------------------------------------------------------
constexpr int PIN_ADC_DIRECT = D2;  // V2 direct path: 'Input' net
constexpr int PIN_ADC_STAGE1 = D0;  // V1 auto-range: OpAmpOut1
constexpr int PIN_ADC_STAGE2 = D1;  // V1 auto-range: OpAmpOut2

constexpr int PIN_ADS_CS = D10;
constexpr int PIN_ADS_RST = D6;
constexpr int PIN_SPI_SCK = D9;
constexpr int PIN_SPI_MISO = D8;  // ADS8681 SDO-0 (net MIS0)
constexpr int PIN_SPI_MOSI = D7;  // ADS8681 SDI   (net MIS1)

// ---------------------------------------------------------------------------
//  Analog front end
// ---------------------------------------------------------------------------

// Level shifter (R3 = R6, R4 = R5 = 10k): V_D2 = V_in + 1.65 V, the 1.65 V
// coming from the R1/R2 divider and its TSV794 buffer.
constexpr float DEFAULT_DIRECT_MID_V = 1.65f;
constexpr float DEFAULT_DIRECT_GAIN = 1.0f;

// The ADS8681's AIN_P is wired to RawInput (the probe itself), so 0 V in reads
// as 0 V. Input range code 0x4 = +/-2.56 V, the tightest range that covers
// the front end's +/-1.65 V.
constexpr float DEFAULT_EXT_MID_V = 0.0f;
constexpr float DEFAULT_EXT_GAIN = 1.0f;
constexpr uint8_t DEFAULT_ADS_RANGE = 0x4;

// Gain stages: inverting amplifiers biased at the R24/R27 mid-rail, so
//   V_adc = off + g * V_in   with g < 0.
// Gains are the design values (3.3 V / window width: x5.5 for window 1,
// x2.36 for window 2; R7 = 255k feedback on stage 1).
constexpr float DEFAULT_STAGE_GAIN[2] = {-5.5f, -2.36f};
constexpr float DEFAULT_STAGE_OFF_V[2] = {1.65f, 1.65f};
// Output of a stage whose mux channel is off (no current in its input
// resistor): the R24/R27 mid-rail.
constexpr float DEFAULT_STAGE_IDLE_V = 1.65f;

// Input-referred window of each stage. Window 1 comes from the R19/R20/R21 =
// 5k/2k/5k ladder: 3.3 V * 5/12 .. 3.3 V * 7/12 = 1.375 .. 1.925 V on 'Input',
// i.e. +/-0.275 V at the probe. Window 2 (R15/R16/R17) is the design value,
// 0.95 .. 2.35 V = +/-0.70 V. CAL BANDS replaces both with measured edges.
constexpr float DEFAULT_WINDOW_LO_V[2] = {-0.275f, -0.70f};
constexpr float DEFAULT_WINDOW_HI_V[2] = {0.275f, 0.70f};
// The windows are nested and the mux picks the innermost one, so stage 2 is
// never driven inside window 1: that region is its "hole". An undriven stage
// sits at its idle level, which reads as roughly 0 V, so without the hole an
// idle stage 2 would look valid for small inputs.
constexpr float DEFAULT_HOLE_LO_V[2] = {0.0f, -0.275f};
constexpr float DEFAULT_HOLE_HI_V[2] = {0.0f, 0.275f};

// AUTO-path stage selection (scope.cpp):
// - only try a stage when the direct reading is within this margin of its window,
constexpr float AUTO_WINDOW_MARGIN_V = 0.05f;
// - and accept it only if it agrees with the direct reading to within this.
constexpr float AUTO_MATCH_TOL_V = 0.04f;
// CAL BANDS: a stage counts as driven when it is this far from its idle level,
constexpr float STAGE_ACTIVE_MIN_V = 0.10f;
// and a gap in its driven inputs wider than expected by this much is a hole.
constexpr float HOLE_MIN_EXTRA_V = 0.10f;

// Native ESP32 ADC: widest attenuation, and the span where its readings are
// trustworthy (Espressif: about 0..2.5 V on the C3, 0..3.1 V on the S3).
constexpr adc_attenuation_t NATIVE_ADC_ATTEN = ADC_11db;
constexpr float ADC_LINEAR_MIN_V = 0.10f;
#if CONFIG_IDF_TARGET_ESP32C3
constexpr float ADC_LINEAR_MAX_V = 2.45f;
#else
constexpr float ADC_LINEAR_MAX_V = 3.00f;
#endif

// ADS8681 SCLK. The chip accepts up to 66.7 MHz; 20 MHz leaves margin for the
// ESP32 GPIO-matrix input delay.
constexpr uint32_t ADS_SPI_HZ = 20000000;

// ---------------------------------------------------------------------------
//  Acquisition limits
// ---------------------------------------------------------------------------
constexpr uint32_t MAX_SAMPLES = 4096;  // record length per channel
constexpr uint32_t MIN_SAMPLES = 16;
constexpr uint32_t MAX_SAMPLE_RATE = 1000000;  // ADS8681 maximum (1 MSPS)
constexpr uint32_t MIN_SAMPLE_RATE = 1;
