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
#include <esp_wifi.h>
#include <esp32-hal-cpu.h>
#include "Settings.h"
#include "Bq25896.h"
#include "SampleGate.h"
#include "PeripheralPolicy.h"
#include "GnssDutyPolicy.h"
#include "Compass.h"
#include "Imu.h"
#include <atomic>

namespace {
Power::Profile sProfile = Power::Profile::Performance;
// The other half of what the peripherals are told, beside the profile above.
// False at boot because the screen is lit when begin() runs. Written on the
// display task inside the section below and read from the GNSS task without
// it; an atomic rather than a plain bool so that stays a deliberate choice
// under link-time optimisation rather than one the compiler has been left
// free to make differently. Relaxed: the value is the whole message, and the
// section it is written under is there for update()'s latches, not for this.
std::atomic<bool> sScreenDark{false};
PeripheralPolicy sPeripherals;
float    sVolts = 0;
// One cadence for whichever battery reader this board has (SampleGate.h):
// the first ask samples, every later one within BATTERY_SAMPLE_MS answers
// from the cache.
SampleGate sSampleGate(BATTERY_SAMPLE_MS);
// When a conversion last actually succeeded, as opposed to when one was last
// attempted. The difference is the whole point: the gate above says the
// sampler is running, and this says it is learning anything by doing so.
uint32_t sLastGoodMs = 0;
bool     sStaleWarned = false;

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

// One task in the battery reader at a time. Four tasks ask for the battery —
// the panel, telemetry, the console and the web server — and each reader has
// its own window when two of them cross the sampling boundary together. On a
// board whose divider sits behind an enable line, one released the line while
// the other was still averaging: a floating pin, scaled and cached as the
// cell for ten seconds. On a PMU board the window is the cache itself: the
// gate advances before the I2C read and the cache fills field-by-field, so
// the caller that skipped the sample copied it half-written — all zeroes,
// "no battery", at the first ask after boot.
#if HAS_BATTERY_ADC || HAS_PMU
SemaphoreHandle_t sSampleLock = nullptr;
#endif

#if HAS_BATTERY_ADC
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
    sLastGoodMs = millis() ? millis() : 1;
    if (sStaleWarned) {
      sStaleWarned = false;
      log_i("battery: the converter on GPIO %d is answering again", PIN_BATTERY_ADC);
    }
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      log_w("battery: the converter on GPIO %d answered nothing — on an ADC2 pin that is "
            "contention with the radio, not a flat cell; holding the last reading",
            PIN_BATTERY_ADC);
    }
  }
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

// The role's ordinals reach the GNSS duty rule as a plain byte, the way the
// profile reaches PeripheralPolicy: that header is host code and this one is
// Arduino code, and neither should have to include the other to be tested.
// What must not happen is the two vocabularies drifting apart — a renumbering
// here would silently change what a stored 1 means on every deployed node — so
// they are pinned against each other in the one file that sees both.
static_assert((uint8_t)Role::Unset     == GnssDutyPolicy::kRoleUnset,     "node role ordinals have drifted");
static_assert((uint8_t)Role::Carried   == GnssDutyPolicy::kRoleCarried,   "node role ordinals have drifted");
static_assert((uint8_t)Role::Transport == GnssDutyPolicy::kRoleTransport, "node role ordinals have drifted");
// And the numbers themselves, not only that the two headers agree about them.
// The three above catch one side moving; they say nothing about both sides
// moving together, which is the renumbering that would keep every build green
// while changing what a 1 already written into a deployed node's NVS means.
static_assert((uint8_t)Role::Carried   == 1, "persisted node role ordinal");
static_assert((uint8_t)Role::Transport == 2, "persisted node role ordinal");

const char* roleName(Role r) {
  // Anything this build does not recognise reads back as "unset", which is
  // also how every rule treats it: a role a later firmware wrote and this one
  // has never heard of is a node whose behaviour nobody here decided.
  switch (r) {
    case Role::Carried:   return "carried";
    case Role::Transport: return "transport";
    default:              return "unset";
  }
}

bool roleFromName(const char* n, Role& out) {
  if (!n) return false;
  // Case-insensitively, as every other named value this node takes.
  if (!strcasecmp(n, "unset"))     { out = Role::Unset;     return true; }
  if (!strcasecmp(n, "carried"))   { out = Role::Carried;   return true; }
  if (!strcasecmp(n, "transport")) { out = Role::Transport; return true; }
  return false;
}

Role role() { return (Role)settings.transport().nodeRole; }

void applyWifiSleep() {
  // Battery goes all the way to max modem sleep: the station dozes
  // wifi.sta_listen_interval beacon intervals between wakes instead of waking
  // for every DTIM, trading inbound latency on the maintenance path for the
  // profile's whole point. Balanced keeps min modem (DTIM), performance none.
  const wifi_ps_type_t want =
      sProfile == Profile::Performance ? WIFI_PS_NONE
    : sProfile == Profile::Battery     ? WIFI_PS_MAX_MODEM
    :                                    WIFI_PS_MIN_MODEM;
  // WiFi.setSleep() only reaches esp_wifi_set_ps() while the STA interface is
  // started — otherwise it just caches the request, and only STA_START ever
  // applies the cache (WiFiGeneric.cpp). An AP-only node, the default shape,
  // never starts STA, so the cache is where the profile's choice would end.
  // The driver is therefore told directly as well; the Arduino call still
  // runs first, so the core's cache — and its own re-apply on a later
  // STA_START — agrees with what was set here.
  WiFi.setSleep(want);
  // Before the driver exists this fails, and that is fine: the
  // STA_START/AP_START hook (WifiManager::begin) runs this again the moment
  // an interface comes up.
  (void)esp_wifi_set_ps(want);
}

void applyWifiTxPower() {
  // dBm to the driver's quarter-dBm; the setting's 2-20 dBm bound
  // (SettingsRules) keeps the product inside the legal [8,84]. The driver
  // quantizes DOWN to its own steps ({8,20,28,34,44,52,56,60,66,72,80}
  // quarter-dBm), so what sticks can sit up to ~1.5 dB below what was asked —
  // wifiTxPowerDbm() below reads back the honest figure. Fails harmlessly
  // before the driver is started (ESP_ERR_WIFI_NOT_START); the
  // STA_START/AP_START hook (WifiManager::begin) re-applies it then.
  (void)esp_wifi_set_max_tx_power((int8_t)(settings.wifi().txPowerDbm * 4));
}

const char* wifiPsName() {
  // esp_wifi_get_ps() answers whether or not the driver is up — the IDF
  // header documents it returning only ESP_OK — so a node with Wi-Fi off
  // would otherwise report the driver's default (min_modem) as if a radio it
  // is not running were saving power. Whether Wi-Fi runs at all is the links
  // settings' rule (Settings.h); ask it rather than re-deriving it here.
  if (!settings.links().wifiEnabled()) return "n/a";
  wifi_ps_type_t ps;
  if (esp_wifi_get_ps(&ps) != ESP_OK) return "n/a";
  switch (ps) {
    case WIFI_PS_NONE:      return "none";
    case WIFI_PS_MIN_MODEM: return "min_modem";
    default:                return "max_modem";
  }
}

float wifiTxPowerDbm() {
  // The same off-rule as wifiPsName(): the getter answers whether or not the
  // driver runs, and a node with Wi-Fi off must not report a ceiling for a
  // radio it is not running.
  if (!settings.links().wifiEnabled()) return NAN;
  int8_t quarter = 0;
  if (esp_wifi_get_max_tx_power(&quarter) != ESP_OK) return NAN;
  return quarter / 4.0f;
}

namespace {
// The broadcast. Both things that decide what a peripheral off the data path
// should be doing — the screen and the profile — come through here, so the
// rule (PeripheralPolicy.h) is asked in one place and each part is told only
// when the answer for it actually moved. `dark` and `prof` are whichever of
// the two inputs this event moved; the other is passed null and left as it
// stands, read inside the section below rather than at the call site.
//
// Under a critical section, because three tasks reach this and the state it
// walks is not one flag. The display task arrives on a screen edge; the main
// loop arrives on a settings commit from the console or from LXMF; the
// async_tcp task arrives on a transport POST from the portal. update() is a
// read-modify-write on its own latches, so two of those interleaved inside it
// could hand one part a verdict and lose the other's, or issue the same
// verdict twice — and sScreenDark is a plain bool written on one task and read
// on another. The blast radius is small, since the next screen edge
// re-converges, but a settings POST landing on a blank is precisely the moment
// this milestone exists for. Same shape as recordHistory() below, and for the
// same reason.
//
// The setRunning() calls stay outside it: each only stores a flag, and nothing
// that is not this state belongs inside a section that stops the scheduler on
// this core. Every one of them is guarded by the part's own board switch, so
// the ten boards carrying neither compile this down to the policy's own
// arithmetic and the section around it — 160 bytes of flash and no RAM,
// measured on heltec-v3, which carries neither part. On the board that carries
// both, the main loop applies the accelerometer's first — the order that
// matters, since the magnetometer reads it for gravity and must neither ask an
// accelerometer that has just gone away nor be woken before one.
portMUX_TYPE sPeripheralMux = portMUX_INITIALIZER_UNLOCKED;

void tellPeripherals(const bool* dark, const Power::Profile* prof) {
  taskENTER_CRITICAL(&sPeripheralMux);
  if (dark) sScreenDark.store(*dark, std::memory_order_relaxed);
  if (prof) sProfile    = *prof;
  const PeripheralPolicy::Change c [[maybe_unused]] =
      sPeripherals.update(sScreenDark.load(std::memory_order_relaxed), (uint8_t)sProfile);
  taskEXIT_CRITICAL(&sPeripheralMux);
#if HAS_COMPASS
  if (c.compass != PeripheralPolicy::Verdict::Unchanged)
    Compass::setRunning(c.compass == PeripheralPolicy::Verdict::Run);
#endif
#if HAS_IMU
  if (c.imu != PeripheralPolicy::Verdict::Unchanged)
    Imu::setRunning(c.imu == PeripheralPolicy::Verdict::Run);
#endif
}
} // namespace

void onScreenBlank(bool dark) { tellPeripherals(&dark, nullptr); }

// Read without the section the writer takes. It is a single bool, written on
// the display task and read from the GNSS task ten times a second; the section
// exists to keep update()'s latches consistent, and a reader that catches the
// previous value one pass before the edge lands is a tenth of a second of a
// receiver tracking, not a lost verdict — while taking the section here would
// stop the scheduler on this core for that same reader.
bool screenDark() { return sScreenDark.load(std::memory_order_relaxed); }

// How long the node waits, on its way to sleep, for the parts to stop. Twenty
// loop passes at the drivers' own retry cadence and two hundred at the rate
// the loop actually runs, so a loop that is running at all lands the verdict
// in the first few milliseconds; the cap is there for the loop that is not.
constexpr uint32_t kSleepSettleMs = 200;

void prepareForSleep() {
  // setBlank(true) reached the parts through onScreenBlank() above, and both
  // of them only recorded the request: the register writes land in
  // Compass::poll() and Imu::poll(), on the main loop. Nothing used to wait
  // for them, and nothing had to — esp_deep_sleep_start() follows within
  // microseconds on a board whose charger has nothing to say (HAS_BQ25896 0,
  // which is the M9's case), so the loop task almost certainly never ran.
  // Deep sleep then holds the pins as they stand and the peripheral rail stays
  // up, so the magnetometer went on converting at 200 Hz with maximum
  // oversampling through a menu item named "off".
  //
  // Spun on rather than signalled: running() is a flag each driver writes on
  // its own task once its own write succeeded, so this reads no register,
  // takes no lock either driver uses and adds nothing to the bus the loop is
  // busy with. Both answer false while absent, so a board with neither part
  // leaves this loop on its first test.
  const uint32_t start = millis();
  while ((Compass::running() || Imu::running()) && millis() - start < kSleepSettleMs)
    vTaskDelay(pdMS_TO_TICKS(5));
  if (Compass::running() || Imu::running())
    log_w("power: %s%s%s still converting %u ms after the screen went dark — sleeping "
          "anyway, and deep sleep will hold it that way",
          Compass::running() ? "the magnetometer" : "",
          (Compass::running() && Imu::running()) ? " and " : "",
          Imu::running() ? "the accelerometer" : "", (unsigned)kSleepSettleMs);
  // And the one flush that is worth a write to flash: after this the rail may
  // not be there at all. Deliberately here and not on every blank — the screen
  // goes dark every twenty seconds on a handheld (Compass.cpp). Ordered after
  // the wait so the part that owns those extremes has stopped sampling them.
  Compass::flush();
}

void apply(Profile p) {
  // The profile lands inside the broadcast's critical section, because it is
  // one of that broadcast's two inputs and three tasks reach it. It moves no
  // sensor verdict today and the policy's test says so; it is asked anyway, so
  // that the day a profile does move one there is no second call site to
  // remember and no part left in the state the previous profile chose.
  tellPeripherals(nullptr, &p);
  switch (p) {
    case Profile::Battery:  setCpuFrequencyMhz(80);  break;
    case Profile::Balanced: setCpuFrequencyMhz(160); break;
    default:                setCpuFrequencyMhz(240); break;
  }
  applyWifiSleep();
  log_i("power profile: %s (CPU %u MHz, Wi-Fi sleep %s)", profileName(p), (unsigned)getCpuFrequencyMhz(),
        p == Profile::Performance ? "off" : p == Profile::Battery ? "max" : "min");
}

uint32_t displaySleepMs() {
  return sProfile == Profile::Battery ? DISPLAY_SLEEP_BATTERY_MS : DISPLAY_SLEEP_MS;
}

void begin() {
#if HAS_BATTERY_ADC || HAS_PMU
  sSampleLock = xSemaphoreCreateMutex();   // before any task can ask
#endif
#if HAS_BATTERY_ADC
  adcSetup();
  sample();
  // Primed, not left untouched: the reading above is fresh, so the gate holds
  // its full interval from here instead of sampling again on the first ask.
  sSampleGate.prime(millis());
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

bool readingStale() {
#if HAS_BATTERY_ADC
  return !(sLastGoodMs && (millis() - sLastGoodMs) <= BATTERY_STALE_MS);
#else
  return false;
#endif
}

Battery battery() {
#if HAS_PMU
  // The power-management chip measures the cell itself, and knows things an
  // ADC divider cannot: whether a battery is actually connected, and whether
  // it is charging. Gated the same as the ADC branch below: it sits on I2C,
  // and the mono OLED path paints twice a second — four live transactions
  // per paint, for a number that only changes over minutes, before this.
  // The gate's first ask samples (SampleGate.h): nothing primes this branch,
  // and answering from the zero-initialised cache for the first ten seconds
  // made every PMU board report "no battery" everywhere right after boot.
  //
  // Gate, read, cache-fill and the answer's copy all sit under the sample
  // lock. The gate advances before the I2C read and the cache fills
  // field-by-field, so a second task crossing with the first would skip the
  // sample and copy the cache half-written — at the very first ask, the
  // all-zero "no battery" the priming rule above exists to prevent. Held
  // across the I2C transaction, exactly as sample() holds it across the
  // divider read; the wait is one battery conversion at worst.
  static Battery sCached{};
  if (sSampleLock) xSemaphoreTake(sSampleLock, portMAX_DELAY);
  if (sSampleGate.due(millis())) {
    Pmu::Battery p = Pmu::battery();
    sCached.volts     = p.volts;
    sCached.present   = p.present;
    sCached.charging  = p.charging;
    sCached.chargeKnown = true;       // the chip is asked directly
    sCached.percent   = p.present ? p.percent : 0;
    recordHistory(sCached.present, sCached.percent);
  }
  const Battery out = sCached;        // copied inside the lock, whole
  if (sSampleLock) xSemaphoreGive(sSampleLock);
  return out;
#elif HAS_BATTERY_ADC
  if (sSampleGate.due(millis())) sample();
  Battery b;
  b.volts   = sVolts;
  b.present = sVolts >= BATTERY_MIN_V && sVolts <= BATTERY_MAX_V;
  // Unless the figure is too old to mean anything. Holding the last reading
  // through a missed conversion is right; holding it for ever is not. A
  // converter that has said nothing for minutes leaves the node with no idea
  // what the cell is doing, and the honest answer to "what is the battery
  // doing" is then the same one a board with no divider gives — nothing —
  // rather than a percentage from whenever it last managed to look.
  //
  // This is the failure that matters, because it does not look like one: the
  // node reports a healthy cell at a plausible voltage, indefinitely, and the
  // single log line saying otherwise was printed once, days ago.
  const bool fresh = sLastGoodMs && (millis() - sLastGoodMs) <= BATTERY_STALE_MS;
  if (!fresh) {
    if (!sStaleWarned) {
      sStaleWarned = true;
      log_w("battery: no conversion on GPIO %d has succeeded for %lu s — reporting no "
            "reading rather than a stale %.3f V", PIN_BATTERY_ADC,
            (unsigned long)(BATTERY_STALE_MS / 1000), sVolts);
    }
    b.present = false;
  }
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
