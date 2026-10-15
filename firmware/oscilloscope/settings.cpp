#include "settings.h"

#include <Preferences.h>
#include <math.h>

#include "ads8681.h"
#include "board_config.h"

namespace {

constexpr uint32_t SETTINGS_MAGIC = 0x3143534F;  // "OSC1"
constexpr char NVS_NAMESPACE[] = "scope";
constexpr char NVS_KEY[] = "cfg";

float sanePositive(float v, float fallback) {
  return (isfinite(v) && fabsf(v) > 1e-6f) ? v : fallback;
}

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
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    s.mid_v[ch] = DEFAULT_MID_V;
    s.path_gain[ch] = DEFAULT_FRONTEND_GAIN;
  }
  for (int b = 0; b < 4; b++) s.band_gain[b] = DEFAULT_BAND_GAIN[b];
}

void settingsSanitize(Settings& s) {
  Settings d;
  settingsDefaults(d);
  if (s.mode >= MODE_COUNT) s.mode = d.mode;
  if (s.trig_mode > TRIG_NORMAL) s.trig_mode = d.trig_mode;
  if (s.trig_edge > EDGE_FALLING) s.trig_edge = d.trig_edge;
  if (s.trig_src > TRIG_SRC_HW) s.trig_src = d.trig_src;
  s.sample_rate = constrain(s.sample_rate, MIN_SAMPLE_RATE, MAX_SAMPLE_RATE);
  s.nsamples = constrain(s.nsamples, MIN_SAMPLES, MAX_SAMPLES);
  if (!isfinite(s.trig_level)) s.trig_level = d.trig_level;
  if (!isfinite(s.trig_hyst) || s.trig_hyst < 0.0f) s.trig_hyst = d.trig_hyst;
  if (s.pretrig_pct > 100) s.pretrig_pct = 100;
  if (!ADS8681::isValidRange(s.ads_range)) s.ads_range = d.ads_range;
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    if (!isfinite(s.mid_v[ch])) s.mid_v[ch] = d.mid_v[ch];
    s.path_gain[ch] = sanePositive(s.path_gain[ch], d.path_gain[ch]);
  }
  for (int b = 0; b < 4; b++) s.band_gain[b] = sanePositive(s.band_gain[b], d.band_gain[b]);
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
  static const char* const names[] = {"AUTO", "DIRECT", "EXT", "HW"};
  return ch <= TRIG_SRC_HW ? names[ch] : "?";
}

const char* trigModeName(uint8_t mode) {
  static const char* const names[] = {"FREE", "AUTO", "NORMAL"};
  return mode <= TRIG_NORMAL ? names[mode] : "?";
}
