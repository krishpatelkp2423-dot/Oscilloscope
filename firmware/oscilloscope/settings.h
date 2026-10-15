// ============================================================================
//  settings.h — acquisition settings and calibration, persisted in NVS flash.
// ============================================================================
#pragma once
#include <Arduino.h>

// Which acquisition architecture(s) to sample.
enum Mode : uint8_t {
  MODE_AUTO = 0,    // V1: hardware auto-ranging (comparators + CD4052 + gain)
  MODE_DIRECT = 1,  // V2: level-shifted signal straight into the ESP32 ADC
  MODE_EXT = 2,     // V3: ADS8681 16-bit SAR ADC over SPI
  MODE_ALL = 3,     // all three, time-aligned, for side-by-side comparison
  MODE_COUNT
};

// Channel ids double as indices into the per-path calibration arrays.
enum ChannelId : uint8_t {
  CH_AUTO = 0,
  CH_DIRECT = 1,
  CH_EXT = 2,
  NUM_CHANNELS = 3
};

enum TrigMode : uint8_t {
  TRIG_FREE = 0,    // no trigger, free-running
  TRIG_AUTO = 1,    // trigger if possible, otherwise show an untriggered frame
  TRIG_NORMAL = 2,  // only show triggered frames
};

enum TrigEdge : uint8_t { EDGE_RISING = 0, EDGE_FALLING = 1 };

// Trigger source: a ChannelId, or the LPV7215 hardware comparator.
constexpr uint8_t TRIG_SRC_HW = 3;

struct Settings {
  uint32_t magic;
  uint8_t mode;
  uint8_t trig_mode;
  uint8_t trig_edge;
  uint8_t trig_src;
  uint32_t sample_rate;  // requested, Hz
  uint32_t nsamples;     // record length per channel
  float trig_level;      // volts at the input
  float trig_hyst;       // volts
  uint8_t pretrig_pct;   // 0..100 % of the record before the trigger
  uint8_t ads_range;     // ADS8681 RANGE_SEL code
  uint8_t reserved[2];

  // Calibration. Reconstruction of the input voltage for each path:
  //   AUTO:   V_in = (V_adc - mid_v[AUTO])   / (path_gain[AUTO] * band_gain[code])
  //   DIRECT: V_in = (V_adc - mid_v[DIRECT]) /  path_gain[DIRECT]
  //   EXT:    V_in = (V_ads - mid_v[EXT])    /  path_gain[EXT]
  float mid_v[NUM_CHANNELS];      // ADC-side voltage that means 0 V at the input
  float path_gain[NUM_CHANNELS];  // input -> ADC node gain (excluding band gain)
  float band_gain[4];             // auto-range gain per CD4052 select code
};

void settingsDefaults(Settings& s);
void settingsSanitize(Settings& s);  // clamps everything into a valid range
bool settingsLoad(Settings& s);      // false if nothing valid is stored
bool settingsSave(const Settings& s);
void settingsErase();

const char* modeName(uint8_t mode);
const char* channelName(uint8_t ch);  // also handles TRIG_SRC_HW
const char* trigModeName(uint8_t mode);
