// ============================================================================
//  scope.h — acquisition engine: native ADC paths, ADS8681, trigger, buffers.
// ============================================================================
#pragma once
#include <Arduino.h>

#include "settings.h"

// One captured record, oldest sample first. Data pointers stay valid until the
// next call to acquire().
struct Capture {
  uint8_t nch = 0;
  uint8_t ch[NUM_CHANNELS] = {};  // channel id of each active channel
  uint32_t n = 0;                 // samples per channel
  int32_t trig_index = -1;        // index of the trigger sample, -1 if none
  float sample_rate = 0.0f;       // measured, Hz
  bool has_bands = false;         // per-sample band codes valid (AUTO active)
  const float* data[NUM_CHANNELS] = {};  // indexed by channel id
  const uint8_t* bands = nullptr;
};

enum class AcqResult : uint8_t {
  Ok,
  NoTrigger,   // NORMAL mode, no trigger yet; call again
  Aborted,     // abort_check() returned true (e.g. a command arrived)
  NoChannels,  // none of the selected mode's channels exist on this board
};

// Configures ADC pins, band/trigger inputs, SPI and the ADS8681.
void scopeBegin(const Settings& s);

bool scopeChannelAvailable(uint8_t ch);
bool scopeAdsPresent();
bool scopeAdsRedetect(uint8_t range);
bool scopeAdsSetRange(uint8_t range);

// Live state of the CD4052 select lines (B << 1 | A) and the HW trigger pin.
uint8_t scopeReadBand();
bool scopeReadHwTrigger();

// Captures one record. trig_mode may differ from s.trig_mode (SINGLE forces
// NORMAL). abort_check is polled about once per millisecond.
AcqResult acquire(const Settings& s, uint8_t trig_mode, bool (*abort_check)(), Capture& cap);

// Averages `count` raw readings of a path's ADC node (volts at the ADC pin,
// before any calibration). For CH_AUTO also reports the most common band and
// whether the band stayed the same for every reading.
struct RawReading {
  float mean_v = 0.0f;
  float min_v = 0.0f;
  float max_v = 0.0f;
  uint16_t last_code = 0;  // ADS8681 only
  uint8_t band = 0;
  bool band_stable = true;
};
bool scopeMeasureRaw(uint8_t ch, uint32_t count, RawReading& out);
