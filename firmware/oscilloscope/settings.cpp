#include "settings.h"

#include <Preferences.h>
#include <math.h>

#include "ads8681.h"
#include "board_config.h"

namespace {

constexpr uint32_t SETTINGS_MAGIC = 0x3443534F;  // "OSC4"
constexpr char NVS_NAMESPACE[] = "scope";
constexpr char NVS_KEY[] = "cfg";

float saneGain(float v, float fallback) {
  return (isfinite(v) && fabsf(v) > 1e-6f) ? v : fallback;
}

float saneFinite(float v, float fallback) { return isfinite(v) ? v : fallback; }

}  // namespace

void settingsDefaults(Settings& s) {
  memset(&s, 0, sizeof s);
  s.magic = SETTINGS_MAGIC;
  s.mode = MODE_ALL;
  s.trig_mode = TRIG_AUTO;
  s.trig_edge = EDGE_RISING;
  s.trig_src = CH_DIRECT;
  s.sample_rate = 10000;
  s.nsamples = 1000;
  s.trig_level = 0.0f;
  s.trig_hyst = 0.03f;
  s.pretrig_pct = 25;
  s.ads_range = DEFAULT_ADS_RANGE;
  s.direct_mid_v = DEFAULT_DIRECT_MID_V;
  s.direct_gain = DEFAULT_DIRECT_GAIN;
  s.ext_mid_v = DEFAULT_EXT_MID_V;
  s.ext_gain = DEFAULT_EXT_GAIN;
  for (int b = 0; b < NUM_STAGES; b++) {
    s.stage_gain[b] = DEFAULT_STAGE_GAIN[b];
    s.stage_off_v[b] = DEFAULT_STAGE_OFF_V[b];
    s.window_lo_v[b] = DEFAULT_WINDOW_LO_V[b];
    s.window_hi_v[b] = DEFAULT_WINDOW_HI_V[b];
    s.hole_lo_v[b] = DEFAULT_HOLE_LO_V[b];
    s.hole_hi_v[b] = DEFAULT_HOLE_HI_V[b];
    s.stage_center_ok[b] = 0;  // only CAL BANDS can establish this
  }
  s.stage_idle_v = DEFAULT_STAGE_IDLE_V;
}

void settingsSanitize(Settings& s) {
  Settings d;
  settingsDefaults(d);
  if (s.mode >= MODE_COUNT) s.mode = d.mode;
  if (s.trig_mode > TRIG_NORMAL) s.trig_mode = d.trig_mode;
  if (s.trig_edge > EDGE_FALLING) s.trig_edge = d.trig_edge;
  if (s.trig_src >= NUM_CHANNELS) s.trig_src = d.trig_src;
  s.sample_rate = constrain(s.sample_rate, MIN_SAMPLE_RATE, MAX_SAMPLE_RATE);
  s.nsamples = constrain(s.nsamples, MIN_SAMPLES, MAX_SAMPLES);
  s.trig_level = saneFinite(s.trig_level, d.trig_level);
  if (!isfinite(s.trig_hyst) || s.trig_hyst < 0.0f) s.trig_hyst = d.trig_hyst;
  if (s.pretrig_pct > 100) s.pretrig_pct = 100;
  if (!ADS8681::isValidRange(s.ads_range)) s.ads_range = d.ads_range;
  s.direct_mid_v = saneFinite(s.direct_mid_v, d.direct_mid_v);
  s.direct_gain = saneGain(s.direct_gain, d.direct_gain);
  s.ext_mid_v = saneFinite(s.ext_mid_v, d.ext_mid_v);
  s.ext_gain = saneGain(s.ext_gain, d.ext_gain);
  for (int b = 0; b < NUM_STAGES; b++) {
    s.stage_gain[b] = saneGain(s.stage_gain[b], d.stage_gain[b]);
    s.stage_off_v[b] = saneFinite(s.stage_off_v[b], d.stage_off_v[b]);
    s.window_lo_v[b] = saneFinite(s.window_lo_v[b], d.window_lo_v[b]);
    s.window_hi_v[b] = saneFinite(s.window_hi_v[b], d.window_hi_v[b]);
    s.hole_lo_v[b] = saneFinite(s.hole_lo_v[b], d.hole_lo_v[b]);
    s.hole_hi_v[b] = saneFinite(s.hole_hi_v[b], d.hole_hi_v[b]);
    s.stage_center_ok[b] = s.stage_center_ok[b] ? 1 : 0;
  }
  s.stage_idle_v = saneFinite(s.stage_idle_v, d.stage_idle_v);
}

bool settingsLoad(Settings& s) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  Settings stored;
  bool ok = prefs.isKey(NVS_KEY) &&
            prefs.getBytesLength(NVS_KEY) == sizeof stored &&
            prefs.getBytes(NVS_KEY, &stored, sizeof stored) == sizeof stored &&
            stored.magic == SETTINGS_MAGIC;
  prefs.end();
  if (!ok) return false;
  s = stored;
  settingsSanitize(s);
  return true;
}

bool settingsSave(const Settings& s) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBytes(NVS_KEY, &s, sizeof s) == sizeof s;
  prefs.end();
  return ok;
}

void settingsErase() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.clear();
  prefs.end();
}

const char* modeName(uint8_t mode) {
  static const char* const names[] = {"AUTO", "DIRECT", "EXT", "ALL"};
  return mode < MODE_COUNT ? names[mode] : "?";
}

const char* channelName(uint8_t ch) {
  static const char* const names[] = {"AUTO", "DIRECT", "EXT"};
  return ch < NUM_CHANNELS ? names[ch] : "?";
}

const char* trigModeName(uint8_t mode) {
  static const char* const names[] = {"FREE", "AUTO", "NORMAL"};
  return mode <= TRIG_NORMAL ? names[mode] : "?";
}
