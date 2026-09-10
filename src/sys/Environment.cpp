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
#if HAS_ENV2
#include "SensirionRhtMath.h"
#endif
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

// Where the part answered. Fixed at ENV_ADDR on every board that knows, and
// settled by begin()'s probe on a board whose sensor arrives on a module and
// brings its own SDO strap (Config.h, ENV_ADDR_ALT). Written once in begin(),
// on the loop task, before any reading is taken; read by everything below.
uint8_t sAddr = ENV_ADDR;

int  readReg(uint8_t reg)                          { return I2cReg::read(bus(), sAddr, reg); }
bool readRegs(uint8_t reg, uint8_t* out, size_t n) { return I2cReg::readN(bus(), sAddr, reg, out, n); }
bool writeReg(uint8_t reg, uint8_t val)            { return I2cReg::write(bus(), sAddr, reg, val); }

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

#if HAS_ENV2
// ---------------------------------------------------------------------------
// The second part: an SHTC3-class humidity sensor beside the barometer
// ---------------------------------------------------------------------------
// Its own scheduling state, not its own scheduling *rule*: EnvPollPolicy and
// SampleGate are the same implementations the BME280 uses, instantiated twice.
// Two parts on one bus must not both convert on the same pass — each gets its
// own gate and they are offset below — but "which pass is this" is one rule
// and stays one rule.
enum class Kind2 : uint8_t { None = 0, Shtc3 = 1, Sht3x = 2 };
Kind2   sKind2  = Kind2::None;
uint8_t sAddr2  = 0;
bool    sPresent2 = false;
Environment::Reading sLast2;
portMUX_TYPE sMux2 = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> sMissed2{0};
SampleGate sGate2(ENV_SAMPLE_MS);
// Whichever family answered, the longer of the two waits is correct for
// either: waiting past a finished conversion costs one pass of a loop that
// runs hundreds of times a second, whereas reading early returns nothing.
// Written as the maximum of the two constants rather than as whichever is
// currently larger, so that a datasheet correction to either one is picked up
// here instead of leaving a hand-chosen figure behind.
constexpr uint32_t kConvert2Ms = Shtc3::kConversionMs > Sht3x::kConversionMs
                               ? Shtc3::kConversionMs : Sht3x::kConversionMs;
EnvPollPolicy sSequence2(kConvert2Ms);

TwoWire& bus2() { return I2cReg::busFor(PIN_ENV_SDA, PIN_ENV_SCL, I2C_HZ); }

bool cmd2(uint16_t c) { return I2cReg::writeCmd16(bus2(), sAddr2, c); }

// An SHTC3 answers its identity word; an SHT3x has no such command that every
// variant implements, so its status register stands in. Either way the proof
// is the same and it is not the address: a checksum that checks out over bytes
// the part chose. An I2C multiplexer also lives at 0x70 and will acknowledge
// there — it cannot produce this.
bool probeShtc3(uint8_t addr) {
  sAddr2 = addr;
  if (!cmd2(Shtc3::kCmdWake)) return false;
  // The part's wake-up time, 240 us typical in the datasheet, rounded up.
  // Microseconds rather than delay(1), and busy-waited rather than sequenced:
  // poll()'s promise is that it does not block *waiting on a conversion*, which
  // is the thirteen milliseconds handled by the policy below. Three hundred
  // microseconds is shorter than a single SPI frame to the panel and does not
  // yield, so splitting it into a third phase would complicate the sequencing
  // to save less time than the check would cost.
  delayMicroseconds(300);
  if (!cmd2(Shtc3::kCmdReadId)) return false;
  uint8_t r[3];
  if (!I2cReg::readRaw(bus2(), addr, r, sizeof(r))) return false;
  if (!SensirionRht::wordOk(r)) { cmd2(Shtc3::kCmdSleep); return false; }
  if (!Shtc3::isShtc3(SensirionRht::word(r))) {
    cmd2(Shtc3::kCmdSleep);
    log_w("env2: the part at 0x%02x checksums correctly but its identity word "
          "is 0x%04x, which is not an SHTC3 — left off rather than decoded as "
          "one", addr, (unsigned)SensirionRht::word(r));
    return false;
  }
  cmd2(Shtc3::kCmdSleep);                          // it idles asleep between readings
  return true;
}

bool probeSht3x(uint8_t addr) {
  sAddr2 = addr;
  if (!cmd2(Sht3x::kCmdStatus)) return false;
  uint8_t r[3];
  if (!I2cReg::readRaw(bus2(), addr, r, sizeof(r))) return false;
  return SensirionRht::wordOk(r);
}

// Order matters and is the bench's: the V4's module answers at 0x70, so that
// is tried first and costs one transaction on a board that has it. The SHT3x
// pair is the universal half — another expansion module could carry one, and a
// firmware that only knew 0x70 would report no sensor on a board that has one.
void beginSecondary() {
  if (probeShtc3(Shtc3::kAddr))          { sKind2 = Kind2::Shtc3; }
  else if (probeSht3x(Sht3x::kAddrLow))  { sKind2 = Kind2::Sht3x; }
  else if (probeSht3x(Sht3x::kAddrHigh)) { sKind2 = Kind2::Sht3x; }
  else {
    sAddr2 = 0;
    log_i("env2: no SHTC3 at 0x%02x and no SHT3x at 0x%02x or 0x%02x — I2C on "
          "the console lists what does answer", Shtc3::kAddr,
          Sht3x::kAddrLow, Sht3x::kAddrHigh);
    return;
  }
  sPresent2 = true;
  // Offset from the barometer's gate by half an interval, so the two parts do
  // not take the bus in the same pass. Nothing breaks if they do — each
  // transaction is bracketed and the loop is the only reader — but a panel
  // sharing this bus would rather have the traffic spread than bunched.
  //
  // Primed half an interval in the *past*, which is what leaves half of one
  // still to run before the first reading. The subtraction can wrap at boot,
  // when millis() is smaller than the offset, and that is harmless for the
  // same reason the gate's own comparison is: it is unsigned arithmetic on a
  // difference, so it stays correct across the wrap.
  sGate2.prime((uint32_t)(millis() - ENV_SAMPLE_MS / 2));
  log_i("env2: %s at 0x%02x, one reading every %u s",
        sKind2 == Kind2::Shtc3 ? "SHTC3" : "SHT3x", sAddr2,
        (unsigned)(ENV_SAMPLE_MS / 1000));
}

bool trigger2() {
  if (sKind2 == Kind2::Shtc3) {
    if (!cmd2(Shtc3::kCmdWake)) return false;
    delayMicroseconds(300);                        // wake-up time; see probeShtc3
    return cmd2(Shtc3::kCmdMeasure);
  }
  return cmd2(Sht3x::kCmdMeasure);
}

// Six bytes: temperature word and its checksum, humidity word and its
// checksum. Both halves must check — half a reply that survived the wire is
// not a reading, and publishing the good half beside an invented one with
// nothing to tell them apart is worse than publishing neither.
bool collect2(Environment::Reading& out) {
  uint8_t d[6];
  const bool got = I2cReg::readRaw(bus2(), sAddr2, d, sizeof(d));
  // Back to sleep whichever way the read went, so the failure paths leave the
  // part in the same state the success path does. Idle is about 45 uA against
  // 0.3 uA asleep — nothing on this board, but a part left awake by a fault is
  // a difference between two runs that nobody chose.
  if (sKind2 == Kind2::Shtc3) cmd2(Shtc3::kCmdSleep);
  if (!got) return false;
  if (!SensirionRht::measurementOk(d)) return false;

  const uint16_t rawT = SensirionRht::word(d);
  const uint16_t rawH = SensirionRht::word(d + 3);
  const int32_t  centiC = (sKind2 == Kind2::Shtc3)
                        ? Shtc3::temperatureCentiC(rawT)
                        : Sht3x::temperatureCentiC(rawT);
  const uint32_t q1024  = (sKind2 == Kind2::Shtc3)
                        ? Shtc3::humidityQ1024(rawH)
                        : Sht3x::humidityQ1024(rawH);
  out.valid       = true;
  out.tempC       = (float)centiC / 100.0f;
  out.humidityPct = (float)q1024 / 1024.0f;
  out.pressureHpa = 0.0f;
  out.hasPressure = false;                        // no barometer in either part
  out.atMs        = millis();
  return true;
}

void pollSecondary() {
  if (!sPresent2) return;
  const uint32_t now = millis();
  const bool inFlight = sSequence2.converting();
  switch (sSequence2.decide(now, !inFlight && sGate2.due(now))) {
    case EnvPollPolicy::Action::Idle:
    case EnvPollPolicy::Action::Wait:
      return;
    case EnvPollPolicy::Action::Trigger: {
      const bool ok = trigger2();
      sSequence2.triggered(now, ok);
      if (!ok && sMissed2.fetch_add(1, std::memory_order_relaxed) + 1 == kComplainAfter)
        log_w("env2: the humidity part has refused to start a conversion %u "
              "times running — it answered at boot, so this is the bus or the "
              "part rather than the wiring", (unsigned)kComplainAfter);
      return;
    }
    case EnvPollPolicy::Action::Collect:
      break;
  }
  sSequence2.finished();

  Environment::Reading r;
  if (!collect2(r)) {
    // A failed checksum lands here as well as a failed transfer, and both mean
    // the same thing to a reader: this interval produced nothing.
    if (sMissed2.fetch_add(1, std::memory_order_relaxed) + 1 == kComplainAfter)
      log_w("env2: %u readings running failed to arrive or failed their "
            "checksum — no reading is being produced", (unsigned)kComplainAfter);
    return;
  }
  taskENTER_CRITICAL(&sMux2);
  sLast2 = r;
  taskEXIT_CRITICAL(&sMux2);
  sMissed2.store(0, std::memory_order_relaxed);
}
#endif // HAS_ENV2

// One address, carried all the way through: chip id, calibration, the three
// coefficients that must not be zero, and the configuration write. All of it,
// or nothing — a board that tries two addresses must not be able to half-
// accept the first and then finish on the second.
bool tryAddress(uint8_t addr) {
  sAddr = addr;
  const Bme280::Part part = Bme280::identify(readReg(kRegChipId));
  if (part != Bme280::Part::Bme280) {
    if (part == Bme280::Part::Bmp280) {
      // Worth telling apart rather than reporting as absent: the pressure and
      // temperature halves of this part are the same and would work, and only
      // the humidity is missing. Nothing here reads one yet, so it is left off
      // and named instead of half-driven on a board nobody has.
      log_w("env: the part at 0x%02x is a BMP280, not a BME280 — no humidity "
            "channel, and this driver wants all three", addr);
    }
    return false;
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
          addr);
    return false;
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
  if (!Bme280::calibrationLooksReal(sCalib)) {
    log_w("env: BME280 at 0x%02x returned an incomplete calibration "
          "(t1=%u p1=%u h1=%u — none can be zero on a real part) — left off "
          "rather than publishing a clamp as a reading", addr,
          (unsigned)sCalib.t1, (unsigned)sCalib.p1, (unsigned)sCalib.h1);
    return false;
  }

  if (!writeReg(kRegConfig, kConfig)) {
    log_w("env: BME280 at 0x%02x would not take its configuration — left off",
          addr);
    return false;
  }
  return true;
}

} // namespace

namespace Environment {

void begin() {
  // The board's own address first, always. On a board that knows where its
  // part sits this is the whole of the search, and ENV_ADDR_ALT is -1 so the
  // second leg does not exist at all.
  bool found = tryAddress((uint8_t)ENV_ADDR);

#if ENV_ADDR_ALT >= 0
  // The other strap. Only reached when the first address produced nothing this
  // driver would drive, and it is held to exactly the same proof — chip id and
  // a real calibration — so "found at the second address" means the same thing
  // as "found at the first" rather than meaning "something was there".
  if (!found) found = tryAddress((uint8_t)ENV_ADDR_ALT);
#endif

  if (!found) {
    // The address reported from here on is the board's first choice again, so
    // a later log line names where the part was looked for rather than
    // wherever the search happened to stop.
    sAddr = (uint8_t)ENV_ADDR;
#if ENV_ADDR_ALT >= 0
    log_i("env: no BME280 at 0x%02x or 0x%02x — I2C on the console lists what "
          "does answer", (uint8_t)ENV_ADDR, (uint8_t)ENV_ADDR_ALT);
#else
    log_i("env: no BME280 answers at 0x%02x — I2C on the console lists what "
          "does", (uint8_t)ENV_ADDR);
#endif
#if HAS_ENV2
    // After the line above, not before it, so the boot log reads in the order
    // the search happened. This is the log somebody reads during bring-up and
    // an out-of-order pair of lines is exactly what sends them looking at the
    // wrong part.
    //
    // The second part is independent of the first: a module with a humidity
    // sensor and no barometer is a board that reports humidity, not a board
    // with no sensors — so this search happens whether or not the BME280
    // answered.
    beginSecondary();
#endif
    return;
  }

  sPresent = true;
  log_i("env: BME280 at 0x%02x, one forced conversion every %u s",
        sAddr, (unsigned)(ENV_SAMPLE_MS / 1000));

#if HAS_ENV2
  beginSecondary();
#endif
}

bool present(Source s) {
#if HAS_ENV2
  if (s == Source::Secondary) return sPresent2;
#else
  (void)s;
#endif
  return s == Source::Primary && sPresent;
}

const char* partName(Source s) {
  if (s == Source::Primary) return sPresent ? "bme280" : "none";
#if HAS_ENV2
  if (!sPresent2) return "none";
  return sKind2 == Kind2::Shtc3 ? "shtc3" : "sht3x";
#else
  return "none";
#endif
}

uint32_t ageS(const Reading& r) {
  return r.valid ? (uint32_t)((millis() - r.atMs) / 1000) : 0;
}

uint32_t missedIntervals(Source s) {
#if HAS_ENV2
  if (s == Source::Secondary) return sMissed2.load(std::memory_order_relaxed);
#elif HAS_ENV
  // A board with one part has nothing to say about a second, and must not
  // answer for it with the first part's figure. present() and partName()
  // already refuse Secondary here; these two used to fall through and hand
  // back the barometer's numbers under the humidity part's name.
  if (s == Source::Secondary) return 0;
#endif
  return sMissed.load(std::memory_order_relaxed);
}

Reading last(Source s) {
  Reading r;
#if HAS_ENV2
  if (s == Source::Secondary) {
    taskENTER_CRITICAL(&sMux2);
    r = sLast2;
    taskEXIT_CRITICAL(&sMux2);
    return r;
  }
#elif HAS_ENV
  // See missedIntervals: an absent part answers as absent, not as the other
  // one. `valid` is false in a default Reading, which is what every surface
  // already draws as "no reading".
  if (s == Source::Secondary) return r;
#endif
  taskENTER_CRITICAL(&sMux);
  r = sLast;
  taskEXIT_CRITICAL(&sMux);
  return r;
}

void poll() {
#if HAS_ENV2
  // Before the barometer's own early returns, not after them: pollPrimary
  // leaves by several paths and a board with no BME280 leaves by the first, so
  // anything appended to the end of it would never run on exactly the board
  // that needs it most.
  pollSecondary();
#endif
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
  r.hasPressure = true;                  // this part is the one with a barometer
  r.humidityPct = (float)Bme280::humidityQ1024(sCalib, tf, raw.humidity) / 1024.0f;
  r.atMs        = now;

  taskENTER_CRITICAL(&sMux);
  sLast = r;
  taskEXIT_CRITICAL(&sMux);
}

} // namespace Environment

#endif // HAS_ENV
