#include "scope.h"

#include <SPI.h>
#include <algorithm>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "ads8681.h"
#include "board_config.h"

namespace {

ADS8681 g_ads;
bool g_ads_ok = false;

// Sample storage, used as a ring buffer while waiting for the trigger.
float g_data[NUM_CHANNELS][MAX_SAMPLES];
uint8_t g_band[MAX_SAMPLES];

// Input-voltage reconstruction coefficients, precomputed per capture.
struct Recon {
  float mid[NUM_CHANNELS];
  float inv_gain[NUM_CHANNELS];
  float inv_band[4];  // 1 / (path_gain[AUTO] * band_gain[code])
};

// One time-aligned sample of every active input.
struct Sample {
  float v[NUM_CHANNELS];
  uint8_t band;
  uint8_t hw;
};

inline float nativeVolts(int pin) { return analogReadMilliVolts(pin) * 0.001f; }

inline uint8_t readBand() {
  uint8_t code = 0;
  if (PIN_BAND_A >= 0 && gpio_get_level((gpio_num_t)PIN_BAND_A)) code |= 1;
  if (PIN_BAND_B >= 0 && gpio_get_level((gpio_num_t)PIN_BAND_B)) code |= 2;
  return code;
}

inline uint8_t readHw() {
  return PIN_HW_TRIGGER >= 0 && gpio_get_level((gpio_num_t)PIN_HW_TRIGGER);
}

// The window comparators can switch the CD4052 while the ADC is converting,
// in which case the reading mixes two gain bands. Detect that by reading the
// select lines on both sides of the conversion and re-sample once.
inline float readAutoPath(const Recon& r, uint8_t& band) {
  uint8_t before = readBand();
  float v = nativeVolts(PIN_ADC_AUTO);
  uint8_t after = readBand();
  if (after != before) v = nativeVolts(PIN_ADC_AUTO);
  band = after;
  return (v - r.mid[CH_AUTO]) * r.inv_band[after];
}

inline void readNative(const bool* use, const Recon& r, Sample& s) {
  s.band = 0;
  if (use[CH_AUTO]) s.v[CH_AUTO] = readAutoPath(r, s.band);
  if (use[CH_DIRECT]) {
    s.v[CH_DIRECT] = (nativeVolts(PIN_ADC_DIRECT) - r.mid[CH_DIRECT]) * r.inv_gain[CH_DIRECT];
  }
  s.hw = readHw();
}

void configureNativeAdc(int pin) {
  if (pin < 0) return;
  analogSetPinAttenuation(pin, NATIVE_ADC_ATTEN);
  analogReadMilliVolts(pin);  // first read sets up the channel; keep it out of captures
}

void configureInput(int pin) {
  if (pin >= 0) pinMode(pin, INPUT);
}

}  // namespace

void scopeBegin(const Settings& s) {
  analogReadResolution(12);
  configureNativeAdc(PIN_ADC_DIRECT);
  configureNativeAdc(PIN_ADC_AUTO);
  configureInput(PIN_BAND_A);
  configureInput(PIN_BAND_B);
  configureInput(PIN_HW_TRIGGER);

  if (PIN_ADS_CS >= 0) {
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
    scopeAdsRedetect(s.ads_range);
  }
}

bool scopeAdsRedetect(uint8_t range) {
  if (PIN_ADS_CS < 0) return false;
  g_ads_ok = g_ads.begin(SPI, PIN_ADS_CS, PIN_ADS_RST, ADS_SPI_HZ);
  if (g_ads_ok) g_ads_ok = g_ads.setRange(range);
  return g_ads_ok;
}

bool scopeChannelAvailable(uint8_t ch) {
  switch (ch) {
    case CH_AUTO: return PIN_ADC_AUTO >= 0;
    case CH_DIRECT: return PIN_ADC_DIRECT >= 0;
    case CH_EXT: return g_ads_ok;
    default: return false;
  }
}

bool scopeAdsPresent() { return g_ads_ok; }

bool scopeAdsSetRange(uint8_t range) { return g_ads_ok && g_ads.setRange(range); }

uint8_t scopeReadBand() { return readBand(); }

bool scopeReadHwTrigger() { return readHw(); }

AcqResult acquire(const Settings& s, uint8_t trig_mode, bool (*abort_check)(), Capture& cap) {
  // ---- Channels -------------------------------------------------------------
  bool use[NUM_CHANNELS] = {
      s.mode == MODE_AUTO || s.mode == MODE_ALL,
      s.mode == MODE_DIRECT || s.mode == MODE_ALL,
      s.mode == MODE_EXT || s.mode == MODE_ALL,
  };
  cap.nch = 0;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    use[ch] = use[ch] && scopeChannelAvailable(ch);
    if (use[ch]) cap.ch[cap.nch++] = ch;
    cap.data[ch] = g_data[ch];
  }
  if (cap.nch == 0) return AcqResult::NoChannels;
  const bool ext = use[CH_EXT];

  // ---- Trigger --------------------------------------------------------------
  uint8_t src = s.trig_src;
  if (src == TRIG_SRC_HW ? PIN_HW_TRIGGER < 0 : !use[src]) src = cap.ch[0];
  const bool hw_src = src == TRIG_SRC_HW;
  const float level = hw_src ? 0.5f : s.trig_level;  // HW pin reads as 0 / 1
  const float hyst = hw_src ? 0.25f : s.trig_hyst;
  const bool rising = s.trig_edge == EDGE_RISING;

  const uint32_t n = s.nsamples;
  uint32_t pre = trig_mode == TRIG_FREE ? 0 : (uint64_t)n * s.pretrig_pct / 100;
  if (pre >= n) pre = n - 1;

  // ---- Timing ---------------------------------------------------------------
  const uint32_t cpu_hz = getCpuFrequencyMhz() * 1000000UL;
  const uint32_t period = std::max<uint32_t>(1, cpu_hz / s.sample_rate);
  const uint32_t frame_ms = (uint64_t)n * 1000 / s.sample_rate;
  const uint32_t timeout_ms = (trig_mode == TRIG_AUTO ? 100 : 250) + 2 * frame_ms;
  const uint32_t check_every = std::max<uint32_t>(1, s.sample_rate / 1000);  // ~1 ms

  // ---- Reconstruction coefficients -------------------------------------------
  Recon r;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    r.mid[ch] = s.mid_v[ch];
    r.inv_gain[ch] = 1.0f / s.path_gain[ch];
  }
  for (int b = 0; b < 4; b++) r.inv_band[b] = 1.0f / (s.path_gain[CH_AUTO] * s.band_gain[b]);

  // The ADS8681 returns each result one frame late (it converts on the CS
  // rising edge and shifts the data out in the next frame). Prime it with one
  // conversion and delay the native readings by one tick to match, so every
  // stored sample has all channels taken at the same instant.
  Sample prev = {};
  if (ext) {
    g_ads.beginCapture();
    g_ads.convert();
    readNative(use, r, prev);
  }

  uint32_t w = 0;                    // ring write index
  uint32_t filled = 0;               // samples stored this call
  uint32_t post_left = UINT32_MAX;   // samples still needed after the trigger
  uint32_t trig_w = 0;
  bool armed = false;
  bool forced = false;               // AUTO-mode timeout: deliver untriggered
  uint32_t check_count = 0;
  AcqResult result = AcqResult::Ok;

  const uint32_t start_ms = millis();
  int64_t t_first = 0, t_last = 0;
  uint32_t next = ESP.getCycleCount() + period;

  for (;;) {
    // Pace on the CPU cycle counter. If we fall behind (an interrupt, or a rate
    // faster than the ADC can go) don't burst to catch up; the real rate is
    // measured below and reported with the frame.
    uint32_t now;
    while ((int32_t)((now = ESP.getCycleCount()) - next) < 0) {
    }
    if ((int32_t)(now - next) > (int32_t)period) next = now;
    next += period;

    t_last = esp_timer_get_time();
    if (filled == 0) t_first = t_last;

    Sample cur;
    float ext_v = 0.0f;
    if (ext) ext_v = (g_ads.codeToVolts(g_ads.convert()) - r.mid[CH_EXT]) * r.inv_gain[CH_EXT];
    readNative(use, r, cur);

    Sample out = cur;
    if (ext) {
      out = prev;
      prev = cur;
      out.v[CH_EXT] = ext_v;
    }

    for (uint8_t k = 0; k < cap.nch; k++) g_data[cap.ch[k]][w] = out.v[cap.ch[k]];
    if (use[CH_AUTO]) g_band[w] = out.band;

    // Edge trigger with hysteresis: arm once the signal is clearly on the far
    // side of the level, fire when it crosses the level.
    if (post_left == UINT32_MAX) {
      if (!forced) {
        bool fire = trig_mode == TRIG_FREE;
        if (!fire) {
          float x = hw_src ? (float)out.hw : out.v[src];
          if (rising ? x < level - hyst : x > level + hyst) {
            armed = true;
          } else if (armed && (rising ? x >= level : x <= level)) {
            fire = true;
            armed = false;
          }
        }
        if (fire && filled >= pre) {
          trig_w = w;
          post_left = n - pre - 1;
        }
      }
    } else {
      post_left--;
    }

    filled++;
    if (++w == n) w = 0;
    if (post_left == 0 || (forced && filled >= n)) break;

    if (++check_count >= check_every) {
      check_count = 0;
      if (abort_check && abort_check()) {
        result = AcqResult::Aborted;
        break;
      }
      if (post_left == UINT32_MAX && !forced && millis() - start_ms > timeout_ms) {
        if (trig_mode == TRIG_AUTO) {
          forced = true;
          if (filled >= n) break;
        } else {
          result = AcqResult::NoTrigger;
          break;
        }
      }
    }
  }

  if (ext) g_ads.endCapture();
  if (result != AcqResult::Ok) return result;

  // The ring is full; its oldest sample sits at the write index. Rotate so
  // the record reads oldest -> newest.
  for (uint8_t k = 0; k < cap.nch; k++) {
    float* d = g_data[cap.ch[k]];
    std::rotate(d, d + w, d + n);
  }
  cap.has_bands = use[CH_AUTO];
  cap.bands = g_band;
  if (cap.has_bands) std::rotate(g_band, g_band + w, g_band + n);

  cap.n = n;
  cap.trig_index = (forced || trig_mode == TRIG_FREE) ? -1 : (int32_t)((trig_w + n - w) % n);
  int64_t elapsed_us = t_last - t_first;
  cap.sample_rate = elapsed_us > 0 ? (filled - 1) * 1e6f / (float)elapsed_us : (float)s.sample_rate;
  return AcqResult::Ok;
}

bool scopeMeasureRaw(uint8_t ch, uint32_t count, RawReading& out) {
  if (!scopeChannelAvailable(ch) || count == 0) return false;
  double sum = 0.0;
  float lo = 1e9f, hi = -1e9f;
  uint32_t hist[4] = {};

  if (ch == CH_EXT) {
    g_ads.beginCapture();
    g_ads.convert();  // prime: the first frame returns a stale result
    delayMicroseconds(5);
  }
  for (uint32_t i = 0; i < count; i++) {
    float v;
    if (ch == CH_EXT) {
      out.last_code = g_ads.convert();
      v = g_ads.codeToVolts(out.last_code);
      delayMicroseconds(5);
    } else if (ch == CH_AUTO) {
      hist[readBand()]++;
      v = nativeVolts(PIN_ADC_AUTO);
    } else {
      v = nativeVolts(PIN_ADC_DIRECT);
    }
    sum += v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  if (ch == CH_EXT) g_ads.endCapture();

  out.mean_v = (float)(sum / count);
  out.min_v = lo;
  out.max_v = hi;
  if (ch == CH_AUTO) {
    out.band = (uint8_t)(std::max_element(hist, hist + 4) - hist);
    out.band_stable = hist[out.band] == count;
  }
  return true;
}
