// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Environment.cpp — see Environment.h
// ============================================================================
#include "Environment.h"

#if HAS_ENV

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include "Bme280Math.h"
#include "EnvPollPolicy.h"
#include "I2cReg.h"
#include "SampleGate.h"

namespace {

// Register map, BME280 datasheet section 5.3.
constexpr uint8_t kRegChipId   = 0xD0;   // 0x60 for a BME280
constexpr uint8_t kRegStatus   = 0xF3;   // bit 3 measuring, bit 0 im_update
constexpr uint8_t kRegCtrlHum  = 0xF2;
constexpr uint8_t kRegCtrlMeas = 0xF4;
constexpr uint8_t kRegConfig   = 0xF5;
constexpr uint8_t kRegData     = 0xF7;   // eight bytes: press, temp, hum
constexpr uint8_t kRegCalib1   = 0x88;   // twenty-six bytes
constexpr uint8_t kRegCalib2   = 0xE1;   // seven bytes

constexpr uint8_t kChipIdBme280 = 0x60;
constexpr uint8_t kChipIdBmp280 = 0x58;  // same part without the humidity half

// Oversampling x1 on all three, and forced mode. x1 because the node is not a
// weather station: the part's own noise at x1 is well inside what the readings
// are used for, and every doubling is conversion time the loop has to wait out
// before it can collect. Forced mode rather than normal, so the part sleeps
// between readings and the cadence is this file's rather than a register's.
constexpr uint8_t kOsrHum   = 0x01;                 // ctrl_hum: osrs_h = x1
constexpr uint8_t kCtrlMeas = (0x01 << 5) |         // osrs_t = x1
                              (0x01 << 2) |         // osrs_p = x1
                              0x01;                 // mode = forced
// Filter off, standby irrelevant in forced mode. The IIR filter is for the
// normal-mode continuous case; with one conversion every half minute it would
// carry a value from the previous reading into this one.
constexpr uint8_t kConfig   = 0x00;

// How long a conversion can take at this oversampling, from the datasheet's
// timing table, rounded up and then doubled. There is no reason to be tight
// about it: the collection happens on a later pass of a loop that runs
// hundreds of times a second, so the cost of waiting twice as long as needed
// is nothing at all, and the status register is read before trusting it.
constexpr uint32_t kConvertMs = 20;

Bme280::Calibration sCalib;
bool sPresent = false;
// Consecutive sampling intervals that produced no reading. Same shape as the
// accelerometer's refused-mode-write counter: complain once at a threshold
// rather than every interval, and let the surfaces say what is actually
// happening instead of "waiting for the first reading" for an hour.
//
// Atomic because it leaves this task. poll() is the only writer and it runs on
// the loop, but missedIntervals() is read by the console, the async web task
// and the display — and a plain uint32_t shared between tasks is a data race
// whatever its width happens to make of it on this chip. Relaxed, because the
// whole message is the value and there is no companion state to order it
// against: the same reasoning, and the same spelling, as this driver's
// neighbours (Compass.cpp, Imu.cpp).
std::atomic<uint32_t> sMissed{0};
constexpr uint32_t kComplainAfter = 3;
SampleGate sGate(ENV_SAMPLE_MS);
// Which of the two passes a reading is, and what a refused trigger means.
// Kept out of here so the sequencing can be tested on a host rather than
// watched on a bench (EnvPollPolicy.h, test/test_env_poll).
EnvPollPolicy sSequence(kConvertMs);

// The last completed reading, and the lock over it. A spinlock rather than a
// mutex because the critical section is a struct copy — the same shape as
// Power's history buffer — and because last() is called from the console task,
// the async web task and the Reticulum task, none of which may block on a
// reading.
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
Environment::Reading sLast;

inline TwoWire& bus() { return I2cReg::busFor(PIN_ENV_SDA, PIN_ENV_SCL, I2C_HZ); }

int  readReg(uint8_t reg)                          { return I2cReg::read(bus(), ENV_ADDR, reg); }
bool readRegs(uint8_t reg, uint8_t* out, size_t n) { return I2cReg::readN(bus(), ENV_ADDR, reg, out, n); }
bool writeReg(uint8_t reg, uint8_t val)            { return I2cReg::write(bus(), ENV_ADDR, reg, val); }

// Ask the part for a single conversion. Its mode bits return to sleep by
// themselves when it finishes, so this is written on every reading rather than
// once in begin().
bool trigger() {
  if (!writeReg(kRegCtrlHum, kOsrHum)) return false;
  return writeReg(kRegCtrlMeas, kCtrlMeas);
}

// True while the part is still converting, or still copying its calibration
// into its image registers. Read before collecting, because "long enough" is a
// timing assumption and this is the part's own answer.
bool busy() {
  const int s = readReg(kRegStatus);
  if (s < 0) return true;                          // no answer: treat as busy
  return (s & 0x09) != 0;                          // measuring | im_update
}

} // namespace

namespace Environment {

void begin() {
  const int id = readReg(kRegChipId);
  if (id != kChipIdBme280) {
    if (id == kChipIdBmp280) {
      // Worth telling apart rather than reporting as absent: the pressure and
      // temperature halves of this part are the same and would work, and only
      // the humidity is missing. Nothing here reads one yet, so it is left off
      // and named instead of half-driven on a board nobody has.
      log_w("env: the part at 0x%02x is a BMP280, not a BME280 — no humidity "
            "channel, and this driver wants all three", (uint8_t)ENV_ADDR);
    } else {
      log_i("env: no BME280 answers at 0x%02x (chip id read %d) — I2C on the "
            "console lists what does", (uint8_t)ENV_ADDR, id);
    }
    return;
  }

  // The calibration is the part's own and is read once. Without it every
  // reading is arithmetic over zeroes, which the polynomial survives — it
  // returns its floor — and which would look like a sensor reporting the
  // coldest, driest, lowest-pressure day on record rather than a failed read.
  uint8_t first[26], second[7];
  if (!readRegs(kRegCalib1, first, sizeof(first)) ||
      !readRegs(kRegCalib2, second, sizeof(second))) {
    log_w("env: BME280 at 0x%02x answered its chip id but would not give up its "
          "calibration — left off rather than reporting arithmetic over zeroes",
          (uint8_t)ENV_ADDR);
    return;
  }
  sCalib = Bme280::decodeCalibration(first, second);

  // A part whose t1 is zero has not given up a real calibration whatever the
  // transfer said: t1 is a large unsigned constant on every real device, and
  // zero is what a block of zeroes decodes to.
  //
  // And the same test on p1 and h1, for the same reason and one that is worse.
  // All three are unsigned in the datasheet's table and none is zero on any
  // real part, so a zero is a transfer that half worked — but where a zero t1
  // would give a floor temperature, a zero p1 is the *divisor* in the pressure
  // polynomial (Bme280Math.h): the guard there returns the 300 hPa clamp, and
  // this driver would then publish that as a valid reading for as long as the
  // node ran. A sensor reporting the lowest pressure ever recorded, steadily,
  // is worse than a sensor reporting nothing, because only one of them makes
  // anybody look at it.
  if (sCalib.t1 == 0 || sCalib.p1 == 0 || sCalib.h1 == 0) {
    log_w("env: BME280 at 0x%02x returned an incomplete calibration "
          "(t1=%u p1=%u h1=%u — none can be zero on a real part) — left off "
          "rather than publishing a clamp as a reading", (uint8_t)ENV_ADDR,
          (unsigned)sCalib.t1, (unsigned)sCalib.p1, (unsigned)sCalib.h1);
    return;
  }

  if (!writeReg(kRegConfig, kConfig)) {
    log_w("env: BME280 at 0x%02x would not take its configuration — left off",
          (uint8_t)ENV_ADDR);
    return;
  }

  sPresent = true;
  log_i("env: BME280 at 0x%02x, one forced conversion every %u s",
        (uint8_t)ENV_ADDR, (unsigned)(ENV_SAMPLE_MS / 1000));
}

bool present() { return sPresent; }

uint32_t ageS(const Reading& r) {
  return r.valid ? (uint32_t)((millis() - r.atMs) / 1000) : 0;
}

uint32_t missedIntervals() { return sMissed.load(std::memory_order_relaxed); }

Reading last() {
  Reading r;
  taskENTER_CRITICAL(&sMux);
  r = sLast;
  taskEXIT_CRITICAL(&sMux);
  return r;
}

void poll() {
  if (!sPresent) return;
  const uint32_t now = millis();

  // Two phases, so that nothing here waits on the part: a pass either starts a
  // conversion or collects one that has had time to finish, and the ten
  // milliseconds in between belong to whatever else the loop was going to do.
  // Which of those this pass is belongs to the policy; what to do about it is
  // the only thing left here.
  //
  // The gate is asked only when nothing is in flight, which is the policy's
  // rule and not this loop's — so it is asked here, in that case, and the
  // answer handed over.
  const bool inFlight = sSequence.converting();
  switch (sSequence.decide(now, !inFlight && sGate.due(now))) {
    case EnvPollPolicy::Action::Idle:
    case EnvPollPolicy::Action::Wait:
      return;
    case EnvPollPolicy::Action::Trigger: {
      const bool ok = trigger();
      sSequence.triggered(now, ok);
      if (!ok && sMissed.fetch_add(1, std::memory_order_relaxed) + 1 == kComplainAfter)
        log_w("env: BME280 has refused to start a conversion %u times running "
              "— it answered at boot, so this is the bus or the part rather "
              "than the wiring", (unsigned)kComplainAfter);
      return;
    }
    case EnvPollPolicy::Action::Collect:
      break;
  }

  // A conversion that is still not finished when its window closes is
  // abandoned rather than read, and waits its turn like any other reading.
  sSequence.finished();
  if (busy()) {
    // Still converting after its whole window. Counted rather than ignored:
    // a part stuck busy for ever is a sensor that never reports, and the only
    // way anybody would know was a surface saying "waiting" indefinitely.
    if (sMissed.fetch_add(1, std::memory_order_relaxed) + 1 == kComplainAfter)
      log_w("env: BME280 has been busy past its conversion window %u times "
            "running — no reading is being produced", (unsigned)kComplainAfter);
    return;
  }

  uint8_t d[8];
  if (!readRegs(kRegData, d, sizeof(d))) {
    // Once at the threshold, not every interval: this is on a bus the panel
    // shares and a log line per half minute for hours is its own problem.
    if (sMissed.fetch_add(1, std::memory_order_relaxed) + 1 == kComplainAfter)
      log_w("env: BME280 finishes its conversions and its data will not read "
            "— %u intervals with no reading", (unsigned)kComplainAfter);
    return;
  }
  sMissed.store(0, std::memory_order_relaxed);
  const Bme280::Raw raw = Bme280::decodeRaw(d);
  // All three from one t_fine, because pressure and humidity are functions of
  // the temperature at the moment of the conversion — three readings computed
  // from three different moments would each be slightly about a different
  // instant, which is exactly the error the burst read exists to avoid.
  const int32_t tf = Bme280::tFine(sCalib, raw.temperature);

  Reading r;
  r.valid       = true;
  r.tempC       = (float)Bme280::temperatureCentiC(tf) / 100.0f;
  r.pressureHpa = (float)Bme280::pressureCentiPa(sCalib, tf, raw.pressure) / 10000.0f;
  r.humidityPct = (float)Bme280::humidityQ1024(sCalib, tf, raw.humidity) / 1024.0f;
  r.atMs        = now;

  taskENTER_CRITICAL(&sMux);
  sLast = r;
  taskEXIT_CRITICAL(&sMux);
}

} // namespace Environment

#endif // HAS_ENV
