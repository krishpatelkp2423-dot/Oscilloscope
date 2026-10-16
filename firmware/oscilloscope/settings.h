// ============================================================================
//  settings.h — acquisition settings and calibration, persisted in NVS flash.
// ============================================================================
#pragma once
#include <Arduino.h>

// Which acquisition architecture(s) to sample.
enum Mode : uint8_t {
  MODE_AUTO = 0,    // V1: auto-range (window comparators + CD4052 + gain stages)
  MODE_DIRECT = 1,  // V2: level-shifted signal straight into the ESP32 ADC
  MODE_EXT = 2,     // V3: ADS8681 16-bit SAR ADC over SPI
  MODE_ALL = 3,     // all three, time-aligned, for side-by-side comparison
  MODE_COUNT
};

// Channel ids; also the trigger-source ids.
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

constexpr uint8_t NUM_STAGES = 2;  // auto-range gain stages

struct Settings {
  uint32_t magic;
  uint8_t mode;
  uint8_t trig_mode;
  uint8_t trig_edge;
  uint8_t trig_src;      // ChannelId
  uint32_t sample_rate;  // requested, Hz
  uint32_t nsamples;     // record length per channel
  float trig_level;      // volts at the input
  float trig_hyst;       // volts
  uint8_t pretrig_pct;   // 0..100 % of the record before the trigger
  uint8_t ads_range;     // ADS8681 RANGE_SEL code
  uint8_t reserved[2];

  // Calibration. Input voltage reconstruction:
  //   DIRECT: V_in = (V_D2  - direct_mid_v) / direct_gain
  //   EXT:    V_in = (V_ads - ext_mid_v)    / ext_gain
  //   stage b (AUTO): V_in = (V_Db - stage_off_v[b]) / stage_gain[b]  (gain < 0)
  float direct_mid_v;
  float direct_gain;
  float ext_mid_v;
  float ext_gain;
  float stage_gain[NUM_STAGES];
  float stage_off_v[NUM_STAGES];
  float window_lo_v[NUM_STAGES];  // input-referred; lo >= hi disables the stage
  float window_hi_v[NUM_STAGES];
  float hole_lo_v[NUM_STAGES];    // input range inside the window where the stage
  float hole_hi_v[NUM_STAGES];    //   is never driven (lo >= hi: no hole)
  float stage_idle_v;             // stage output when its mux channel is off
};

void settingsDefaults(Settings& s);
void settingsSanitize(Settings& s);  // clamps everything into a valid range
bool settingsLoad(Settings& s);      // false if nothing valid is stored
bool settingsSave(const Settings& s);
void settingsErase();

const char* modeName(uint8_t mode);
const char* channelName(uint8_t ch);
const char* trigModeName(uint8_t mode);
