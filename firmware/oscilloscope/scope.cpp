#include "scope.h"

#include <SPI.h>
#include <algorithm>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <math.h>

#include "ads8681.h"
#include "board_config.h"

namespace {

ADS8681 g_ads;
bool g_ads_ok = false;

constexpr int kStagePin[NUM_STAGES] = {PIN_ADC_STAGE1, PIN_ADC_STAGE2};

// Sample storage, used as a ring buffer while waiting for the trigger.
float g_data[NUM_CHANNELS][MAX_SAMPLES];
uint8_t g_band[MAX_SAMPLES];

// Input-voltage reconstruction coefficients, precomputed per capture.
struct Recon {
  float direct_mid, direct_inv_gain;
  float ext_mid, ext_inv_gain;
  uint8_t nstages;             // stages to try, highest |gain| first
  uint8_t order[NUM_STAGES];
  float stage_off[NUM_STAGES];
  float stage_inv_gain[NUM_STAGES];
  float win_lo[NUM_STAGES];
  float win_hi[NUM_STAGES];
  float hole_lo[NUM_STAGES];
  float hole_hi[NUM_STAGES];
};

// One time-aligned sample of every active input.
struct Sample {
  float v[NUM_CHANNELS];
  uint8_t band;
};

Recon makeRecon(const Settings& s) {
  Recon r = {};
  r.direct_mid = s.direct_mid_v;
  r.direct_inv_gain = 1.0f / s.direct_gain;
  r.ext_mid = s.ext_mid_v;
  r.ext_inv_gain = 1.0f / s.ext_gain;
  for (uint8_t b = 0; b < NUM_STAGES; b++) {
    r.stage_off[b] = s.stage_off_v[b];
    r.stage_inv_gain[b] = 1.0f / s.stage_gain[b];
    r.win_lo[b] = s.window_lo_v[b];
    r.win_hi[b] = s.window_hi_v[b];
    r.hole_lo[b] = s.hole_lo_v[b];
    r.hole_hi[b] = s.hole_hi_v[b];
    if (kStagePin[b] >= 0 && s.window_lo_v[b] < s.window_hi_v[b]) r.order[r.nstages++] = b;
  }
  if (r.nstages == 2 && fabsf(s.stage_gain[r.order[1]]) > fabsf(s.stage_gain[r.order[0]])) {
    std::swap(r.order[0], r.order[1]);
  }
  return r;
}

inline float nativeVolts(int pin) { return analogReadMilliVolts(pin) * 0.001f; }

inline float readDirect(const Recon& r) {
  return (nativeVolts(PIN_ADC_DIRECT) - r.direct_mid) * r.direct_inv_gain;
}

// V1 auto-range. The XIAO can't see the mux select lines, so it decides which
// gain stage is live from the readings: try the highest-gain stage whose
// window (minus its hole) contains the direct reading x, and accept it only if
// its value is in the ADC's linear range and agrees with the direct path. The direct path is
// read again after the stage so the comparison uses the average of two reads
// taken either side of it, which cancels the signal's slope to first order.
// If no stage qualifies, fall back to the direct reading (band 0).
inline float readAutoPath(const Recon& r, float x, uint8_t& band) {
  for (uint8_t k = 0; k < r.nstages; k++) {
    uint8_t b = r.order[k];
    if (x < r.win_lo[b] - AUTO_WINDOW_MARGIN_V || x > r.win_hi[b] + AUTO_WINDOW_MARGIN_V) continue;
    if (x > r.hole_lo[b] + AUTO_WINDOW_MARGIN_V && x < r.hole_hi[b] - AUTO_WINDOW_MARGIN_V) continue;
    float y = nativeVolts(kStagePin[b]);
    if (y < ADC_LINEAR_MIN_V || y > ADC_LINEAR_MAX_V) continue;
    float v = (y - r.stage_off[b]) * r.stage_inv_gain[b];
    float x_after = readDirect(r);
    if (fabsf(v - 0.5f * (x + x_after)) > AUTO_MATCH_TOL_V) continue;
    band = b + 1;
    return v;
  }
  band = 0;
  return x;
}

inline void readNative(const bool* use, const Recon& r, Sample& s) {
  s.band = 0;
  if (!use[CH_AUTO] && !use[CH_DIRECT]) return;
  float x = readDirect(r);
  if (use[CH_DIRECT]) s.v[CH_DIRECT] = x;
  if (use[CH_AUTO]) s.v[CH_AUTO] = readAutoPath(r, x, s.band);
}

void configureNativeAdc(int pin) {
  if (pin < 0) return;
  analogSetPinAttenuation(pin, NATIVE_ADC_ATTEN);
  analogReadMilliVolts(pin);  // first read sets up the channel; keep it out of captures
}

// Least-squares fit of y = off + gain * x over the samples where the stage is
// driven (clear of its idle level and inside the ADC's linear range). A second
// pass drops points more than 3 sigma from the first fit (mux switching
// transients).
//
// Hole detection: a driven stage only counts as "active" once it is
// STAGE_ACTIVE_MIN_V from idle, i.e. |x - x0| > STAGE_ACTIVE_MIN_V / |gain|
// around the input x0 that maps to the idle level. A wider gap in the active
// inputs around x0 means the stage isn't driven there at all.
StageFit fitStage(const float* x, const float* y, uint32_t count, float idle_v) {
  StageFit best;
  float gain = 0.0f, off = 0.0f, sigma = INFINITY;
  for (int pass = 0; pass < 2; pass++) {
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    float lo = INFINITY, hi = -INFINITY;
    for (uint32_t i = 0; i < count; i++) {
      if (y[i] < ADC_LINEAR_MIN_V || y[i] > ADC_LINEAR_MAX_V) continue;
      if (fabsf(y[i] - idle_v) < STAGE_ACTIVE_MIN_V) continue;
      if (pass == 1 && fabsf(y[i] - (off + gain * x[i])) > 3.0f * sigma) continue;
      n += 1;
      sx += x[i];
      sy += y[i];
      sxx += (double)x[i] * x[i];
      sxy += (double)x[i] * y[i];
      syy += (double)y[i] * y[i];
      lo = std::min(lo, x[i]);
      hi = std::max(hi, x[i]);
    }
    double den = n * sxx - sx * sx;
    if (n < 100 || den <= 0 || hi - lo < 0.1f) {
      if (pass == 0) best.n = (uint32_t)n;
      break;
    }
    gain = (float)((n * sxy - sx * sy) / den);
    off = (float)((sy - gain * sx) / n);
    double ssr = syy - off * sy - gain * sxy;  // sum of squared residuals at the optimum
    sigma = std::max((float)sqrt(std::max(ssr, 0.0) / n), 0.001f);
    best.ok = true;
    best.n = (uint32_t)n;
    best.gain = gain;
    best.off_v = off;
    best.lo_v = lo;
    best.hi_v = hi;
    best.resid_v = sigma / fabsf(gain);
  }
  if (!best.ok) return best;

  const float x0 = (idle_v - best.off_v) / best.gain;
  float below = -INFINITY, above = INFINITY;  // nearest active inputs either side of x0
  for (uint32_t i = 0; i < count; i++) {
    if (y[i] < ADC_LINEAR_MIN_V || y[i] > ADC_LINEAR_MAX_V) continue;
    if (fabsf(y[i] - idle_v) < STAGE_ACTIVE_MIN_V) continue;
    if (fabsf(y[i] - (best.off_v + best.gain * x[i])) > 3.0f * sigma) continue;
    if (x[i] < x0) below = std::max(below, x[i]);
    else above = std::min(above, x[i]);
  }
  const float expected_gap = 2.0f * STAGE_ACTIVE_MIN_V / fabsf(best.gain) + HOLE_MIN_EXTRA_V;
  if (isfinite(below) && isfinite(above) && above - below > expected_gap) {
    best.hole_lo_v = below;
    best.hole_hi_v = above;
  }
  return best;
}

}  // namespace

void scopeBegin(const Settings& s) {
  analogReadResolution(12);
  configureNativeAdc(PIN_ADC_DIRECT);
  for (int pin : kStagePin) configureNativeAdc(pin);

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
    case CH_AUTO: return PIN_ADC_DIRECT >= 0 && (PIN_ADC_STAGE1 >= 0 || PIN_ADC_STAGE2 >= 0);
    case CH_DIRECT: return PIN_ADC_DIRECT >= 0;
    case CH_EXT: return g_ads_ok;
    default: return false;
  }
}

bool scopeAdsPresent() { return g_ads_ok; }

bool scopeAdsSetRange(uint8_t range) { return g_ads_ok && g_ads.setRange(range); }

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
  const uint8_t src = (s.trig_src < NUM_CHANNELS && use[s.trig_src]) ? s.trig_src : cap.ch[0];
  const float level = s.trig_level;
  const float hyst = s.trig_hyst;
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

  const Recon r = makeRecon(s);

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

    Sample cur = {};
    float ext_v = 0.0f;
    if (ext) ext_v = (g_ads.codeToVolts(g_ads.convert()) - r.ext_mid) * r.ext_inv_gain;
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
          float x = out.v[src];
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

const char* rawSourceName(uint8_t src) {
  static const char* const names[] = {"direct (D2)", "stage 1 (D0)", "stage 2 (D1)", "ADS8681"};
  return src < RAW_COUNT ? names[src] : "?";
}

bool scopeMeasureRaw(uint8_t src, uint32_t count, RawReading& out) {
  int pin = -1;
  switch (src) {
    case RAW_DIRECT: pin = PIN_ADC_DIRECT; break;
    case RAW_STAGE1: pin = PIN_ADC_STAGE1; break;
    case RAW_STAGE2: pin = PIN_ADC_STAGE2; break;
    case RAW_EXT: break;
    default: return false;
  }
  if (count == 0 || (src == RAW_EXT ? !g_ads_ok : pin < 0)) return false;

  double sum = 0.0;
  float lo = INFINITY, hi = -INFINITY;
  if (src == RAW_EXT) {
    g_ads.beginCapture();
    g_ads.convert();  // prime: the first frame returns a stale result
    delayMicroseconds(5);
  }
  for (uint32_t i = 0; i < count; i++) {
    float v;
    if (src == RAW_EXT) {
      out.last_code = g_ads.convert();
      v = g_ads.codeToVolts(out.last_code);
      delayMicroseconds(5);
    } else {
      v = nativeVolts(pin);
    }
    sum += v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  if (src == RAW_EXT) g_ads.endCapture();

  out.mean_v = (float)(sum / count);
  out.min_v = lo;
  out.max_v = hi;
  return true;
}

float scopeAutoReading(const Settings& s, uint8_t& band) {
  const Recon r = makeRecon(s);
  return readAutoPath(r, readDirect(r), band);
}

void scopeFitStages(const Settings& s, uint32_t count, uint32_t rate_hz, StageFit fit[NUM_STAGES]) {
  count = std::min<uint32_t>(count, MAX_SAMPLES);
  const Recon r = makeRecon(s);
  // Reuse the capture buffers: x (input from the direct path) and each stage's
  // raw output.
  float* xs = g_data[0];
  float* ys[NUM_STAGES] = {g_data[1], g_data[2]};
  const uint32_t period_us = std::max<uint32_t>(1, 1000000UL / rate_hz);
  uint32_t next = micros();
  for (uint32_t i = 0; i < count; i++) {
    while ((int32_t)(micros() - next) < 0) {
    }
    next += period_us;
    float x_before = readDirect(r);
    for (uint8_t b = 0; b < NUM_STAGES; b++) {
      ys[b][i] = kStagePin[b] >= 0 ? nativeVolts(kStagePin[b]) : -1.0f;  // -1: never "active"
    }
    float x_after = readDirect(r);
    xs[i] = 0.5f * (x_before + x_after);
  }
  for (uint8_t b = 0; b < NUM_STAGES; b++) fit[b] = fitStage(xs, ys[b], count, s.stage_idle_v);
}
