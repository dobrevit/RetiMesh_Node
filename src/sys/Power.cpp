// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

// ============================================================================
//  Power.cpp — see Power.h
// ============================================================================
#include "Power.h"
#include "Pmu.h"
#include <WiFi.h>
#include <esp32-hal-cpu.h>
#include "Settings.h"
#include "Bq25896.h"

namespace {
Power::Profile sProfile = Power::Profile::Performance;
float    sVolts = 0;
uint32_t sLastSample = 0;

// Single-cell LiPo open-circuit voltage to a rough percentage.
uint8_t percentFor(float v) {
  static const float pts[][2] = { {4.20, 100}, {4.06, 90}, {3.98, 80}, {3.92, 70}, {3.87, 60},
                                  {3.82, 50}, {3.79, 40}, {3.77, 30}, {3.74, 20}, {3.68, 10}, {3.45, 0} };
  if (v >= pts[0][0]) return 100;
  for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) {
    if (v >= pts[i][0]) {
      float f = (v - pts[i][0]) / (pts[i-1][0] - pts[i][0]);
      return (uint8_t)(pts[i][1] + f * (pts[i-1][1] - pts[i][1]));
    }
  }
  return 0;
}

#if HAS_BATTERY_ADC
// One task in the divider at a time. Four tasks ask for the battery — the
// panel, telemetry, the console and the web server — and on a board whose
// divider sits behind an enable line, two of them crossing the staleness
// boundary together meant one released the line while the other was still
// averaging: a floating pin, scaled and cached as the cell for ten seconds.
SemaphoreHandle_t sSampleLock = nullptr;

// The enable line parked once; the attenuation is handled per read, below —
// both orderings of "configure the pin once up front" were tried against the
// hardware and neither survives: analogReadMilliVolts selects its calibration
// curve from the driver-wide attenuation and ignores the per-pin setting, so
// a pin-only configuration read the V4's 0.76 V divider tap as 2.2 V and an
// 11.4 V battery, before and after an attach-first reordering.
void adcSetup() {
#if PIN_BATTERY_ADC_EN >= 0
  pinMode(PIN_BATTERY_ADC_EN, OUTPUT);
  digitalWrite(PIN_BATTERY_ADC_EN, !BATTERY_ADC_EN_ACTIVE);
#endif
}

void sample() {
  // The board says how deep its divider is and at what attenuation the result
  // is readable (Config.h): most halve the cell and read at 11 dB, one divides
  // by five and reads at 2.5 dB. Average a few readings; the pin floats
  // without a cell.
  if (sSampleLock && xSemaphoreTake(sSampleLock, 0) != pdTRUE) return;  // someone is mid-read
  // The driver-wide attenuation is what analogReadMilliVolts calibrates by
  // (measured — the per-pin call alone leaves the curve at 11 dB), so it is
  // swapped in for the reads and restored after: no other analog input ever
  // sees this board's 2.5 dB, which on any later sensor pin would clip a
  // 2.5 V signal at full scale with no error. Both writes sit inside the
  // lock, so no concurrent sampler reads between them.
  analogSetAttenuation(BATTERY_ADC_ATTEN);
  analogSetPinAttenuation(PIN_BATTERY_ADC, BATTERY_ADC_ATTEN);
#if PIN_BATTERY_ADC_EN >= 0
  // The divider is only connected while its enable line is held, so hold it
  // for the reading and release it after: left connected it drains the cell
  // it measures. Reading it unheld does not fail — it returns a plausible
  // number that is not the battery, which is the worse outcome.
  digitalWrite(PIN_BATTERY_ADC_EN, BATTERY_ADC_EN_ACTIVE);
  delay(2);                              // let the divider settle
#endif
  // A read that came back with nothing is not a reading of an empty cell.
  // A divider on ADC2 — the converter the radio and the Wi-Fi stack contend
  // for — loses arbitration now and then and the call returns zero; averaged
  // in with seven good ones that is an eighth of the cell voltage gone, which
  // is a percentage drop an operator can see and a "no battery" once enough of
  // them miss. So the zeros are counted out and, if every one of them missed,
  // the last figure stands rather than being replaced by one nobody measured.
  uint32_t acc = 0;
  uint8_t  got = 0;
  for (int i = 0; i < 8; i++) {
    const uint32_t mv = analogReadMilliVolts(PIN_BATTERY_ADC);
    if (mv) { acc += mv; got++; }
  }
#if PIN_BATTERY_ADC_EN >= 0
  digitalWrite(PIN_BATTERY_ADC_EN, !BATTERY_ADC_EN_ACTIVE);
#endif
  analogSetAttenuation(ADC_11db);        // the default every other reader assumes
  if (got) {
    sVolts = (acc / got) / 1000.0f * BATTERY_DIVIDER_RATIO;
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      log_w("battery: the converter on GPIO %d answered nothing — on an ADC2 pin that is "
            "contention with the radio, not a flat cell; holding the last reading",
            PIN_BATTERY_ADC);
    }
  }
  sLastSample = millis();
  if (sSampleLock) xSemaphoreGive(sSampleLock);
}
#endif
}

namespace Power {

const char* profileName(Profile p) {
  switch (p) { case Profile::Balanced: return "balanced"; case Profile::Battery: return "battery"; default: return "performance"; }
}

bool profileFromName(const char* n, Profile& out) {
  if (!n) return false;
  // Case-insensitively, as every other named value this node takes.
  if (!strcasecmp(n, "performance")) { out = Profile::Performance; return true; }
  if (!strcasecmp(n, "balanced"))    { out = Profile::Balanced;    return true; }
  if (!strcasecmp(n, "battery"))     { out = Profile::Battery;     return true; }
  return false;
}

Profile profile() { return sProfile; }

void apply(Profile p) {
  sProfile = p;
  switch (p) {
    case Profile::Battery:     setCpuFrequencyMhz(80);  WiFi.setSleep(true);  break;
    case Profile::Balanced:    setCpuFrequencyMhz(160); WiFi.setSleep(true);  break;
    default:                   setCpuFrequencyMhz(240); WiFi.setSleep(false); break;
  }
  log_i("power profile: %s (CPU %u MHz, Wi-Fi sleep %s)", profileName(p), (unsigned)getCpuFrequencyMhz(),
        p == Profile::Performance ? "off" : "on");
}

uint32_t displaySleepMs() {
  return sProfile == Profile::Battery ? DISPLAY_SLEEP_BATTERY_MS : DISPLAY_SLEEP_MS;
}

void begin() {
#if HAS_BATTERY_ADC
  // The default for every channel attached from here on, rather than a
  // per-pin setting: core 3 attaches a pin to the ADC on its first read and
  // refuses to configure one that is not attached yet, so the per-pin call
  // before the first sample logged an error and set nothing.
  
#if HAS_BATTERY_ADC
  sSampleLock = xSemaphoreCreateMutex();   // before any task can ask
  adcSetup();
#endif
  sample();
#endif
  apply((Profile)settings.transport().powerProfile);
}

// One point per five minutes, 96 slots — eight hours, the spec's window.
static uint8_t     sHist[96];
static uint32_t    sHistCount = 0;
static uint32_t    sHistLastMs = 0;
static portMUX_TYPE sHistMux = portMUX_INITIALIZER_UNLOCKED;

static void recordHistory(bool present, uint8_t percent) {
  if (!present) return;
  const uint32_t now = millis();
  // The gate lives inside the lock: two callers crossing the five-minute
  // boundary together once double-inserted and quietly compressed the
  // sparkline's clock.
  taskENTER_CRITICAL(&sHistMux);
  if (!sHistCount || now - sHistLastMs >= 300000) {
    sHist[sHistCount % 96] = percent;
    sHistCount++;
    sHistLastMs = now;
  }
  taskEXIT_CRITICAL(&sHistMux);
}

size_t batteryHistory(uint8_t* out, size_t max) {
  taskENTER_CRITICAL(&sHistMux);
  const uint32_t n = sHistCount < 96 ? sHistCount : 96;
  size_t w = 0;
  for (uint32_t i = 0; i < n && w < max; i++)
    out[w++] = sHist[(sHistCount - n + i) % 96];
  taskEXIT_CRITICAL(&sHistMux);
  return w;
}

Battery battery() {
#if HAS_PMU
  // The power-management chip measures the cell itself, and knows things an
  // ADC divider cannot: whether a battery is actually connected, and whether
  // it is charging.
  Pmu::Battery p = Pmu::battery();
  Battery b;
  b.volts    = p.volts;
  b.present  = p.present;
  b.charging = p.charging;
  b.chargeKnown = true;             // the chip is asked directly
  b.percent  = p.present ? p.percent : 0;
  recordHistory(b.present, b.percent);
  return b;
#elif HAS_BATTERY_ADC
  if (millis() - sLastSample > BATTERY_SAMPLE_MS) sample();
  Battery b;
  b.volts   = sVolts;
  b.present = sVolts >= BATTERY_MIN_V && sVolts <= BATTERY_MAX_V;
  b.percent = b.present ? percentFor(sVolts) : 0;
#if HAS_BQ25896
  // The divider still measures the cell; the charger answers the one
  // question the divider cannot.
  if (Bq25896::present()) {
    b.chargeKnown = true;
    b.charging = Bq25896::charging();
  }
#endif
  recordHistory(b.present, b.percent);
  return b;
#else
  // Neither a power-management chip nor a divider: this board cannot see a
  // cell even if one is fitted. Saying "no battery" is the truthful answer and
  // the one every caller already handles — the alternative was not compiling,
  // which is what a board with no sensing used to do.
  return Battery{};
#endif
}

} // namespace Power
