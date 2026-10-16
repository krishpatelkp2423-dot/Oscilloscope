// ============================================================================
//  DIY Oscilloscope firmware — Seeed Studio XIAO ESP32 (C3 / S3 / C6)
//
//  Three acquisition paths share one level-shifting front end:
//    V1 AUTO   — window comparators + CD4052 steer the signal into one of two
//                inverting gain stages (D0, D1); firmware picks the live stage
//                and divides its gain back out.
//    V2 DIRECT — level-shifted signal straight into the ESP32 ADC (D2).
//    V3 EXT    — ADS8681 16-bit SAR ADC on the raw input, over SPI
//                (driver in ads8681.cpp).
//
//  Data and control go over the USB serial port. Send HELP for the command
//  list. FORMAT CSV (the default) prints Arduino Serial Plotter-friendly text;
//  FORMAT BIN switches to the framed binary stream used by host/scope_gui.py.
//
//  Board-specific pins and gains: board_config.h
// ============================================================================
#include <Arduino.h>
#include <math.h>
#include <stdarg.h>

#include "ads8681.h"
#include "board_config.h"
#include "protocol.h"
#include "scope.h"
#include "settings.h"

#define FIRMWARE_VERSION "1.1.0"

namespace {

Settings g_cfg;
Capture g_cap;
bool g_running = false;
bool g_single = false;
bool g_binary = false;  // FORMAT BIN vs CSV; not persisted so a fresh boot is human-readable
uint32_t g_seq = 0;

char g_line[128];
size_t g_line_len = 0;
bool g_line_overflow = false;

// ---------------------------------------------------------------------------
//  Output helpers
// ---------------------------------------------------------------------------

void reply(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void reply(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (g_binary) {
    proto::sendText(Serial, buf);
  } else {
    Serial.println(buf);
  }
}

bool serialPending() { return Serial.available() > 0; }

void sendCaptureBinary(bool single) {
  proto::WaveformHeader h = {};
  h.seq = g_seq++;
  h.mode = g_cfg.mode;
  h.nch = g_cap.nch;
  h.flags = (g_cap.trig_index >= 0 ? proto::FLAG_TRIGGERED : 0) |
            (g_cap.has_bands ? proto::FLAG_HAS_BANDS : 0) |
            (single ? proto::FLAG_SINGLE : 0);
  h.trig_src = g_cfg.trig_src;
  h.nsamples = g_cap.n;
  h.trig_index = g_cap.trig_index;
  h.sample_rate = g_cap.sample_rate;
  h.trig_level = g_cfg.trig_level;
  memset(h.ch_id, 0xFF, sizeof h.ch_id);
  for (uint8_t k = 0; k < g_cap.nch; k++) h.ch_id[k] = g_cap.ch[k];

  const uint32_t data_bytes = (uint32_t)g_cap.nch * g_cap.n * sizeof(float);
  const uint32_t len = sizeof h + data_bytes + (g_cap.has_bands ? g_cap.n : 0);
  proto::PacketWriter w(Serial, proto::PKT_WAVEFORM, len);
  w.write(&h, sizeof h);
  for (uint8_t k = 0; k < g_cap.nch; k++) {
    w.write(g_cap.data[g_cap.ch[k]], g_cap.n * sizeof(float));
  }
  if (g_cap.has_bands) w.write(g_cap.bands, g_cap.n);
  w.finish();
}

// One "label:value,label:value" line per sample: the Arduino IDE Serial
// Plotter draws each label as its own trace.
void sendCaptureCsv() {
  static const char* const labels[NUM_CHANNELS] = {"auto", "direct", "ext"};
  char line[96];
  for (uint32_t i = 0; i < g_cap.n; i++) {
    int pos = 0;
    for (uint8_t k = 0; k < g_cap.nch && pos < (int)sizeof line; k++) {
      uint8_t ch = g_cap.ch[k];
      pos += snprintf(line + pos, sizeof line - pos, "%s%s:%.5f", k ? "," : "", labels[ch],
                      g_cap.data[ch][i]);
    }
    if (g_cap.has_bands && pos < (int)sizeof line) {
      pos += snprintf(line + pos, sizeof line - pos, ",band:%u", g_cap.bands[i]);
    }
    Serial.println(line);
  }
}

// ---------------------------------------------------------------------------
//  Info / help
// ---------------------------------------------------------------------------

void printHelp() {
  reply("Commands (case-insensitive):");
  reply("  RUN | STOP | SINGLE         start, stop, or capture one triggered frame");
  reply("  MODE AUTO|DIRECT|EXT|ALL    acquisition path(s): V1, V2, V3, or all three");
  reply("  RATE <Hz>                   sample rate (actual rate is measured per frame)");
  reply("  N <samples>                 record length, %u..%u", (unsigned)MIN_SAMPLES,
        (unsigned)MAX_SAMPLES);
  reply("  TRIG FREE|AUTO|NORMAL       trigger mode");
  reply("  EDGE RISING|FALLING         trigger edge");
  reply("  LEVEL <V> | HYST <V>        trigger level / hysteresis, input-referred volts");
  reply("  SRC AUTO|DIRECT|EXT         trigger source");
  reply("  PRE <0-100>                 pre-trigger percentage of the record");
  reply("  FORMAT CSV|BIN              text for Serial Plotter / binary for scope_gui.py");
  reply("  ADSRANGE <code>             ADS8681 range: 0-4 bipolar (0x4=+/-2.56V), 8-11 unipolar");
  reply("  MID DIRECT|EXT <V>          node voltage that means 0 V at the input");
  reply("  PGAIN DIRECT|EXT <g>        path gain, input -> ADC node");
  reply("  STAGE <1|2> <gain> [off V]  gain stage: V_adc = off + gain * V_in (gain < 0)");
  reply("  WINDOW <1|2> <lo> <hi> [<hole lo> <hole hi>] | OFF");
  reply("                              input range where a gain stage may be used");
  reply("  CAL ZERO                    input shorted to GND: measure mid-points");
  reply("  CAL GAIN <V>                known DC input: measure DIRECT/EXT gains");
  reply("  CAL BANDS                   slow signal swept over +/-1 V: fit both gain stages");
  reply("  PROBE                       raw reading of every net (board bring-up)");
  reply("  INFO | SAVE | LOAD | DEFAULTS");
}

void printInfo() {
  char range[24];
  ADS8681::describeRange(g_cfg.ads_range, range, sizeof range);
  reply("Oscilloscope fw %s | %s @ %u MHz | arduino-esp32 %d.%d.%d", FIRMWARE_VERSION,
        ESP.getChipModel(), (unsigned)getCpuFrequencyMhz(), ESP_ARDUINO_VERSION_MAJOR,
        ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  reply("mode=%s rate=%lu n=%lu trig=%s edge=%s src=%s level=%.4f hyst=%.4f pre=%u%%",
        modeName(g_cfg.mode), (unsigned long)g_cfg.sample_rate, (unsigned long)g_cfg.nsamples,
        trigModeName(g_cfg.trig_mode), g_cfg.trig_edge == EDGE_RISING ? "RISING" : "FALLING",
        channelName(g_cfg.trig_src), g_cfg.trig_level, g_cfg.trig_hyst, g_cfg.pretrig_pct);
  reply("state=%s format=%s", g_running ? "RUN" : "STOP", g_binary ? "BIN" : "CSV");
  reply("paths: AUTO=%s DIRECT=%s EXT=%s (ADS8681 %s, range %s)",
        scopeChannelAvailable(CH_AUTO) ? "yes" : "no",
        scopeChannelAvailable(CH_DIRECT) ? "yes" : "no",
        scopeChannelAvailable(CH_EXT) ? "yes" : "no",
        scopeAdsPresent() ? "detected" : "NOT detected", range);
  reply("cal direct: mid=%.4f V gain=%.4f | ext: mid=%.5f V gain=%.4f", g_cfg.direct_mid_v,
        g_cfg.direct_gain, g_cfg.ext_mid_v, g_cfg.ext_gain);
  for (uint8_t b = 0; b < NUM_STAGES; b++) {
    if (g_cfg.window_lo_v[b] < g_cfg.window_hi_v[b]) {
      char hole[40] = "";
      if (g_cfg.hole_lo_v[b] < g_cfg.hole_hi_v[b]) {
        snprintf(hole, sizeof hole, " except %+.3f..%+.3f V", g_cfg.hole_lo_v[b],
                 g_cfg.hole_hi_v[b]);
      }
      reply("cal stage %u: gain=%.4f off=%.4f V window %+.3f..%+.3f V%s", b + 1,
            g_cfg.stage_gain[b], g_cfg.stage_off_v[b], g_cfg.window_lo_v[b],
            g_cfg.window_hi_v[b], hole);
    } else {
      reply("cal stage %u: gain=%.4f off=%.4f V window OFF", b + 1, g_cfg.stage_gain[b],
            g_cfg.stage_off_v[b]);
    }
  }
  reply("cal stage idle level=%.4f V", g_cfg.stage_idle_v);
  reply("pins (GPIO): direct=%d stage1=%d stage2=%d ads cs=%d rst=%d sck=%d miso=%d mosi=%d",
        PIN_ADC_DIRECT, PIN_ADC_STAGE1, PIN_ADC_STAGE2, PIN_ADS_CS, PIN_ADS_RST, PIN_SPI_SCK,
        PIN_SPI_MISO, PIN_SPI_MOSI);
}

// Live raw readings of every net, to check the board against the schematic:
// with the input grounded, D2 and both stage outputs should read about 1.65 V
// and the ADS8681 about 0 V.
void probe() {
  if (!scopeAdsPresent()) scopeAdsRedetect(g_cfg.ads_range);
  RawReading rd;
  if (scopeMeasureRaw(RAW_DIRECT, 256, rd)) {
    reply("PROBE direct (D2): node=%.4f V (min %.4f max %.4f) -> input %+.4f V", rd.mean_v,
          rd.min_v, rd.max_v, (rd.mean_v - g_cfg.direct_mid_v) / g_cfg.direct_gain);
  }
  for (uint8_t b = 0; b < NUM_STAGES; b++) {
    if (!scopeMeasureRaw(RAW_STAGE1 + b, 256, rd)) continue;
    reply("PROBE stage %u (D%u): node=%.4f V (min %.4f max %.4f) -> input %+.4f V if driven",
          b + 1, b, rd.mean_v, rd.min_v, rd.max_v,
          (rd.mean_v - g_cfg.stage_off_v[b]) / g_cfg.stage_gain[b]);
  }
  uint8_t band;
  float v = scopeAutoReading(g_cfg, band);
  reply("PROBE auto: input %+.4f V from %s", v,
        band == 0 ? "the direct path (no gain stage agreed)" : band == 1 ? "stage 1" : "stage 2");
  if (scopeMeasureRaw(RAW_EXT, 256, rd)) {
    reply("PROBE ext: code=0x%04X node=%+.5f V (min %+.5f max %+.5f) -> input %+.5f V",
          rd.last_code, rd.mean_v, rd.min_v, rd.max_v,
          (rd.mean_v - g_cfg.ext_mid_v) / g_cfg.ext_gain);
  } else {
    reply("PROBE ext: ADS8681 not detected (check CS/SCLK/SDI/SDO, 5 V AVDD, 3.3 V DVDD)");
  }
}

// ---------------------------------------------------------------------------
//  Calibration
// ---------------------------------------------------------------------------

void calZero() {
  RawReading rd;
  bool any = false;
  if (scopeMeasureRaw(RAW_DIRECT, 2048, rd)) {
    g_cfg.direct_mid_v = rd.mean_v;
    any = true;
    reply("CAL ZERO direct: mid=%.5f V (noise p-p %.2f mV)", rd.mean_v,
          (rd.max_v - rd.min_v) * 1000.0f);
  }
  if (scopeMeasureRaw(RAW_EXT, 2048, rd)) {
    g_cfg.ext_mid_v = rd.mean_v;
    any = true;
    reply("CAL ZERO ext: mid=%+.5f V (noise p-p %.3f mV)", rd.mean_v,
          (rd.max_v - rd.min_v) * 1000.0f);
  }
  // 0 V sits inside window 1, so stage 2's mux channel is off: its output is
  // the idle level that CAL BANDS uses to tell driven from undriven samples.
  if (scopeMeasureRaw(RAW_STAGE2, 2048, rd)) {
    g_cfg.stage_idle_v = rd.mean_v;
    any = true;
    reply("CAL ZERO stage idle (D1): %.5f V", rd.mean_v);
  }
  if (scopeMeasureRaw(RAW_STAGE1, 2048, rd)) {
    reply("CAL ZERO stage 1 (D0) at 0 V: %.5f V (equals the idle level if stage 1 is driven "
          "there and its offset is small)", rd.mean_v);
  }
  reply(any ? "OK CAL ZERO done; SAVE to keep it" : "ERR no channels to calibrate");
}

void calGain(float known_v) {
  if (!isfinite(known_v) || fabsf(known_v) < 0.05f) {
    reply("ERR apply and pass a known DC voltage of at least 50 mV, e.g. CAL GAIN 1.000");
    return;
  }
  struct Path {
    uint8_t src;
    const char* name;
    float mid;
    float* gain;
  } paths[] = {
      {RAW_DIRECT, "DIRECT", g_cfg.direct_mid_v, &g_cfg.direct_gain},
      {RAW_EXT, "EXT", g_cfg.ext_mid_v, &g_cfg.ext_gain},
  };
  for (const Path& p : paths) {
    RawReading rd;
    if (!scopeMeasureRaw(p.src, 2048, rd)) continue;
    float delta = rd.mean_v - p.mid;
    if (fabsf(delta) < 0.01f) {
      reply("CAL GAIN %s: skipped, node moved only %.1f mV from mid", p.name, delta * 1000.0f);
      continue;
    }
    *p.gain = delta / known_v;
    reply("CAL GAIN %s: path gain=%.5f", p.name, *p.gain);
  }
  reply("OK CAL GAIN done; run CAL BANDS next for the auto-range stages, then SAVE");
}

void calBands() {
  reply("CAL BANDS: sampling for 2 s; keep a slow signal (<= 50 Hz) sweeping across +/-1 V");
  StageFit fit[NUM_STAGES];
  scopeFitStages(g_cfg, 4000, 2000, fit);
  for (uint8_t b = 0; b < NUM_STAGES; b++) {
    const StageFit& f = fit[b];
    if (!f.ok) {
      g_cfg.window_lo_v[b] = g_cfg.window_hi_v[b] = 0.0f;
      reply("CAL BANDS stage %u: only %lu driven samples; stage disabled. Is its CD4052 channel "
            "ever selected? (see README: nested windows select Y3)", b + 1, (unsigned long)f.n);
      continue;
    }
    g_cfg.stage_gain[b] = f.gain;
    g_cfg.stage_off_v[b] = f.off_v;
    g_cfg.window_lo_v[b] = f.lo_v;
    g_cfg.window_hi_v[b] = f.hi_v;
    g_cfg.hole_lo_v[b] = f.hole_lo_v;
    g_cfg.hole_hi_v[b] = f.hole_hi_v;
    reply("CAL BANDS stage %u: gain=%.4f off=%.4f V window %+.3f..%+.3f V, fit residual "
          "%.2f mV rms over %lu samples", b + 1, f.gain, f.off_v, f.lo_v, f.hi_v,
          f.resid_v * 1000.0f, (unsigned long)f.n);
    if (f.hole_lo_v < f.hole_hi_v) {
      reply("CAL BANDS stage %u: not driven for %+.3f..%+.3f V (inner window)", b + 1,
            f.hole_lo_v, f.hole_hi_v);
    }
  }
  reply("OK CAL BANDS done; SAVE to keep it");
}

// ---------------------------------------------------------------------------
//  Command parsing
// ---------------------------------------------------------------------------

bool parseFloat(const char* tok, float& out) {
  if (!tok) return false;
  char* end;
  out = strtof(tok, &end);
  return end != tok && *end == '\0' && isfinite(out);
}

bool parseUint(const char* tok, uint32_t& out) {
  if (!tok || *tok == '-') return false;
  char* end;
  out = strtoul(tok, &end, 0);  // base 0: accepts 11, 0xB, ...
  return end != tok && *end == '\0';
}

int lookup(const char* tok, const char* const* names, int count) {
  if (!tok) return -1;
  for (int i = 0; i < count; i++) {
    if (strcasecmp(tok, names[i]) == 0) return i;
  }
  return -1;
}

const char* const kModeNames[] = {"AUTO", "DIRECT", "EXT", "ALL"};
const char* const kTrigNames[] = {"FREE", "AUTO", "NORMAL"};
const char* const kEdgeNames[] = {"RISING", "FALLING"};
const char* const kSrcNames[] = {"AUTO", "DIRECT", "EXT"};
const char* const kCalPathNames[] = {"DIRECT", "EXT"};

void handleCommand(char* line) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " \t");
  char* a2 = strtok(nullptr, " \t");
  char* a3 = strtok(nullptr, " \t");
  char* a4 = strtok(nullptr, " \t");
  char* a5 = strtok(nullptr, " \t");
  for (char* p = cmd; *p; p++) *p = toupper(*p);

  uint32_t u;
  float f, f2, f3, f4;
  int idx;

  if (!strcmp(cmd, "HELP") || !strcmp(cmd, "?")) {
    printHelp();
  } else if (!strcmp(cmd, "INFO")) {
    printInfo();
  } else if (!strcmp(cmd, "RUN")) {
    g_running = true;
    g_single = false;
    reply("OK RUN");
  } else if (!strcmp(cmd, "STOP")) {
    g_running = false;
    g_single = false;
    reply("OK STOP");
  } else if (!strcmp(cmd, "SINGLE")) {
    g_running = true;
    g_single = true;
    reply("OK SINGLE armed");
  } else if (!strcmp(cmd, "MODE")) {
    if ((idx = lookup(a1, kModeNames, MODE_COUNT)) < 0) return reply("ERR MODE AUTO|DIRECT|EXT|ALL");
    g_cfg.mode = idx;
    reply("OK MODE %s", modeName(g_cfg.mode));
  } else if (!strcmp(cmd, "RATE")) {
    if (!parseUint(a1, u) || u < MIN_SAMPLE_RATE || u > MAX_SAMPLE_RATE) {
      return reply("ERR RATE %lu..%lu", (unsigned long)MIN_SAMPLE_RATE,
                   (unsigned long)MAX_SAMPLE_RATE);
    }
    g_cfg.sample_rate = u;
    reply("OK RATE %lu", (unsigned long)u);
  } else if (!strcmp(cmd, "N")) {
    if (!parseUint(a1, u) || u < MIN_SAMPLES || u > MAX_SAMPLES) {
      return reply("ERR N %u..%u", (unsigned)MIN_SAMPLES, (unsigned)MAX_SAMPLES);
    }
    g_cfg.nsamples = u;
    reply("OK N %lu", (unsigned long)u);
  } else if (!strcmp(cmd, "TRIG")) {
    if ((idx = lookup(a1, kTrigNames, 3)) < 0) return reply("ERR TRIG FREE|AUTO|NORMAL");
    g_cfg.trig_mode = idx;
    reply("OK TRIG %s", trigModeName(idx));
  } else if (!strcmp(cmd, "EDGE")) {
    if ((idx = lookup(a1, kEdgeNames, 2)) < 0) return reply("ERR EDGE RISING|FALLING");
    g_cfg.trig_edge = idx;
    reply("OK EDGE %s", kEdgeNames[idx]);
  } else if (!strcmp(cmd, "LEVEL")) {
    if (!parseFloat(a1, f)) return reply("ERR LEVEL <volts>");
    g_cfg.trig_level = f;
    reply("OK LEVEL %.4f", f);
  } else if (!strcmp(cmd, "HYST")) {
    if (!parseFloat(a1, f) || f < 0) return reply("ERR HYST <volts >= 0>");
    g_cfg.trig_hyst = f;
    reply("OK HYST %.4f", f);
  } else if (!strcmp(cmd, "SRC")) {
    if ((idx = lookup(a1, kSrcNames, NUM_CHANNELS)) < 0) return reply("ERR SRC AUTO|DIRECT|EXT");
    g_cfg.trig_src = idx;
    reply("OK SRC %s", channelName(idx));
  } else if (!strcmp(cmd, "PRE")) {
    if (!parseUint(a1, u) || u > 100) return reply("ERR PRE 0..100");
    g_cfg.pretrig_pct = u;
    reply("OK PRE %lu", (unsigned long)u);
  } else if (!strcmp(cmd, "FORMAT")) {
    if (a1 && !strcasecmp(a1, "BIN")) {
      g_binary = true;
    } else if (a1 && !strcasecmp(a1, "CSV")) {
      g_binary = false;
    } else {
      return reply("ERR FORMAT CSV|BIN");
    }
    reply("OK FORMAT %s", g_binary ? "BIN" : "CSV");
  } else if (!strcmp(cmd, "ADSRANGE")) {
    if (!parseUint(a1, u) || !ADS8681::isValidRange(u)) return reply("ERR ADSRANGE 0-4 or 8-11");
    if (!scopeAdsSetRange(u)) return reply("ERR ADS8681 not responding");
    g_cfg.ads_range = u;
    char range[24];
    ADS8681::describeRange(u, range, sizeof range);
    reply("OK ADSRANGE 0x%lX (%s)", (unsigned long)u, range);
  } else if (!strcmp(cmd, "MID") || !strcmp(cmd, "PGAIN")) {
    bool mid = !strcmp(cmd, "MID");
    if ((idx = lookup(a1, kCalPathNames, 2)) < 0 || !parseFloat(a2, f) || (!mid && f == 0.0f)) {
      return reply("ERR %s DIRECT|EXT <%s>", cmd, mid ? "volts" : "gain");
    }
    float* target = mid ? (idx == 0 ? &g_cfg.direct_mid_v : &g_cfg.ext_mid_v)
                        : (idx == 0 ? &g_cfg.direct_gain : &g_cfg.ext_gain);
    *target = f;
    reply("OK %s %s %.5f", cmd, kCalPathNames[idx], f);
  } else if (!strcmp(cmd, "STAGE")) {
    if (!parseUint(a1, u) || u < 1 || u > NUM_STAGES || !parseFloat(a2, f) || f == 0.0f ||
        (a3 && !parseFloat(a3, f2))) {
      return reply("ERR STAGE <1|2> <gain> [offset volts]");
    }
    g_cfg.stage_gain[u - 1] = f;
    if (a3) g_cfg.stage_off_v[u - 1] = f2;
    reply("OK STAGE %lu gain=%.4f off=%.4f", (unsigned long)u, g_cfg.stage_gain[u - 1],
          g_cfg.stage_off_v[u - 1]);
  } else if (!strcmp(cmd, "WINDOW")) {
    if (!parseUint(a1, u) || u < 1 || u > NUM_STAGES) {
      return reply("ERR WINDOW <1|2> <lo> <hi> [<hole lo> <hole hi>] | WINDOW <1|2> OFF");
    }
    if (a2 && !strcasecmp(a2, "OFF")) {
      g_cfg.window_lo_v[u - 1] = g_cfg.window_hi_v[u - 1] = 0.0f;
      return reply("OK WINDOW %lu OFF", (unsigned long)u);
    }
    if (!parseFloat(a2, f) || !parseFloat(a3, f2) || f >= f2 ||
        (a4 && (!parseFloat(a4, f3) || !parseFloat(a5, f4) || f3 >= f4))) {
      return reply("ERR WINDOW <1|2> <lo> <hi> [<hole lo> <hole hi>] with lo < hi");
    }
    g_cfg.window_lo_v[u - 1] = f;
    g_cfg.window_hi_v[u - 1] = f2;
    g_cfg.hole_lo_v[u - 1] = a4 ? f3 : 0.0f;
    g_cfg.hole_hi_v[u - 1] = a4 ? f4 : 0.0f;
    reply("OK WINDOW %lu %+.3f..%+.3f hole %+.3f..%+.3f", (unsigned long)u, f, f2,
          g_cfg.hole_lo_v[u - 1], g_cfg.hole_hi_v[u - 1]);
  } else if (!strcmp(cmd, "CAL")) {
    if (a1 && !strcasecmp(a1, "ZERO")) {
      calZero();
    } else if (a1 && !strcasecmp(a1, "GAIN") && parseFloat(a2, f)) {
      calGain(f);
    } else if (a1 && !strcasecmp(a1, "BANDS")) {
      calBands();
    } else {
      reply("ERR CAL ZERO | CAL GAIN <volts> | CAL BANDS");
    }
  } else if (!strcmp(cmd, "PROBE")) {
    probe();
  } else if (!strcmp(cmd, "SAVE")) {
    reply(settingsSave(g_cfg) ? "OK SAVE" : "ERR SAVE failed");
  } else if (!strcmp(cmd, "LOAD")) {
    if (!settingsLoad(g_cfg)) return reply("ERR nothing saved");
    scopeAdsSetRange(g_cfg.ads_range);
    reply("OK LOAD");
  } else if (!strcmp(cmd, "DEFAULTS")) {
    settingsDefaults(g_cfg);
    settingsErase();
    scopeAdsSetRange(g_cfg.ads_range);
    reply("OK DEFAULTS (saved settings erased)");
  } else {
    reply("ERR unknown command '%s' (try HELP)", cmd);
  }
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!g_line_overflow && g_line_len > 0) {
        g_line[g_line_len] = '\0';
        handleCommand(g_line);
      }
      g_line_len = 0;
      g_line_overflow = false;
    } else if (g_line_len < sizeof g_line - 1) {
      g_line[g_line_len++] = c;
    } else {
      g_line_overflow = true;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(921600);  // the baud rate is ignored over native USB
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);

  settingsDefaults(g_cfg);
  bool loaded = settingsLoad(g_cfg);
  scopeBegin(g_cfg);

  reply("Oscilloscope fw %s ready (%s settings). Send HELP for commands, RUN to start.",
        FIRMWARE_VERSION, loaded ? "saved" : "default");
  if (!scopeAdsPresent()) reply("WARN ADS8681 not detected; EXT path disabled");
}

void loop() {
  pollSerial();
  if (!g_running) {
    delay(2);
    return;
  }

  // SINGLE waits for a real trigger even when the trigger mode is AUTO.
  uint8_t trig_mode = g_cfg.trig_mode;
  if (g_single && trig_mode == TRIG_AUTO) trig_mode = TRIG_NORMAL;

  AcqResult r = acquire(g_cfg, trig_mode, serialPending, g_cap);
  if (r == AcqResult::Ok) {
    if (g_binary) {
      sendCaptureBinary(g_single);
    } else {
      sendCaptureCsv();
    }
    if (g_single) {
      g_running = false;
      g_single = false;
    }
  } else if (r == AcqResult::NoChannels) {
    reply("ERR mode %s has no working channel on this board; stopping", modeName(g_cfg.mode));
    g_running = false;
  }
  delay(1);  // let lower-priority tasks run between frames
}
