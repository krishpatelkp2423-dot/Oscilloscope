// ============================================================================
//  scope.h — acquisition engine: native ADC paths, ADS8681, trigger, buffers.
// ============================================================================
#pragma once
#include <Arduino.h>

#include "settings.h"

// One captured record, oldest sample first. Data pointers stay valid until the
// next call to acquire() or scopeFitStages().
struct Capture {
  uint8_t nch = 0;
  uint8_t ch[NUM_CHANNELS] = {};  // channel id of each active channel
  uint32_t n = 0;                 // samples per channel
  int32_t trig_index = -1;        // index of the trigger sample, -1 if none
  uint8_t trig_src = 0;           // channel the trigger actually used
  float sample_rate = 0.0f;       // measured, Hz
  bool has_bands = false;         // per-sample band codes valid (AUTO active)
  const float* data[NUM_CHANNELS] = {};  // indexed by channel id
  const uint8_t* bands = nullptr;        // 0 = direct (x1), 1 = stage 1, 2 = stage 2
};

enum class AcqResult : uint8_t {
  Ok,
  NoTrigger,   // NORMAL mode, no trigger yet; call again
  Aborted,     // abort_check() returned true (e.g. a command arrived)
  NoChannels,  // none of the selected mode's channels exist on this board
};

// Configures the ADC pins, SPI and the ADS8681.
void scopeBegin(const Settings& s);

bool scopeChannelAvailable(uint8_t ch);
bool scopeAdsPresent();
bool scopeAdsRedetect(uint8_t range);
bool scopeAdsSetRange(uint8_t range);

// Captures one record. trig_mode may differ from s.trig_mode (SINGLE forces
// NORMAL). abort_check is polled about once per millisecond.
AcqResult acquire(const Settings& s, uint8_t trig_mode, bool (*abort_check)(), Capture& cap);

// Raw (uncalibrated) node voltages, for bring-up and calibration.
enum RawSource : uint8_t { RAW_DIRECT, RAW_STAGE1, RAW_STAGE2, RAW_EXT, RAW_COUNT };
struct RawReading {
  float mean_v = 0.0f;
  float min_v = 0.0f;
  float max_v = 0.0f;
  uint16_t last_code = 0;  // ADS8681 only
};
bool scopeMeasureRaw(uint8_t src, uint32_t count, RawReading& out);
const char* rawSourceName(uint8_t src);

// One auto-range reading: the reconstructed input and which path produced it.
float scopeAutoReading(const Settings& s, uint8_t& band);

// CAL BANDS: samples the direct path and both gain stages `count` times at
// `rate_hz`, then fits V_stage = off + gain * V_in by least squares over the
// samples where each stage is being driven, and finds the input range where it
// was driven (window) and where inside it the mux never drives it (hole).
struct StageFit {
  bool ok = false;
  uint32_t n = 0;        // samples used
  float gain = 0.0f;     // signed (negative for the inverting stages)
  float off_v = 0.0f;    // stage output at 0 V input
  float lo_v = 0.0f;     // input range over which the stage was driven
  float hi_v = 0.0f;
  float hole_lo_v = 0.0f;  // inputs inside the window where the mux never
  float hole_hi_v = 0.0f;  //   drives the stage (lo == hi: none)
  bool center_ok = false;  // driven around its idle point (see readAutoPath)
  float resid_v = 0.0f;    // RMS fit residual, input-referred
};
// sweep_lo/hi: the input range the signal covered during the measurement.
void scopeFitStages(const Settings& s, uint32_t count, uint32_t rate_hz, StageFit fit[NUM_STAGES],
                    float& sweep_lo, float& sweep_hi);
