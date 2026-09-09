// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Compass.cpp — see Compass.h
//
//  Two parts, one driver. QST's QMC6309 and QMC6310 share a register map —
//  chip id at 0x00, six little-endian output bytes at 0x01, status at 0x09,
//  mode in CTRL1 and range in CTRL2 — and disagree about the fields inside
//  those two registers and about the microtesla a count is worth. So this file
//  is the conversation and src/sys/QmcMag.h is the numbers, chosen by
//  COMPASS_KIND, and everything below this line is the same for both.
//
//  The QMC6309's map is from QST's part and from a reading of the part itself:
//  `I2C 0x7c` on the console answered chip id 0x90 at register 0, with both
//  control registers at zero — which is this part saying it is suspended and
//  nobody has configured it. That is the state begin() takes it out of.
//
//  Note that address. 0x7c is inside the block the I2C specification reserves
//  for ten-bit addressing, which is why the bus scan had to be widened past the
//  conventional 0x77 to see this part at all. QST put it there; the scan works
//  around it (I2cReg.h). The QMC6310N sits at 0x3c instead, in ordinary space
//  and one address below a common OLED panel's — which on the T-Beam Supreme
//  is exactly where the panel's driver went looking, initialised a
//  magnetometer, and left the glass showing the previous firmware's picture.
// ============================================================================
#include "Compass.h"

#if HAS_COMPASS

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Preferences.h>
#include "I2cReg.h"
#include "Imu.h"
#include "MagHeading.h"
#include "QmcMag.h"
#include "SampleGate.h"
#include <atomic>

namespace {

constexpr uint8_t kChipId   = QmcMag::kRegChipId;
constexpr uint8_t kDataX    = QmcMag::kRegDataX;   // six bytes, little-endian x, y, z
constexpr uint8_t kStatus   = QmcMag::kRegStatus;
constexpr uint8_t kCtrl1    = QmcMag::kRegCtrl1;
constexpr uint8_t kCtrl2    = QmcMag::kRegCtrl2;
constexpr uint8_t kSoftReset = QmcMag::kSoftReset;

// Which part, and therefore which numbers. Both run continuously at 200 Hz —
// faster than this file samples them, deliberately: the part is always holding
// a conversion that finished milliseconds ago, so a sample never waits and
// never catches one half-made. They differ in what else that byte carries,
// which is QmcMag.h's business and not this file's.
#if COMPASS_KIND == COMPASS_KIND_QMC6310
constexpr uint8_t kWantChipId = QmcMag::kChipIdQmc6310;
constexpr uint8_t kCtrl1Run   = QmcMag::kQmc6310Ctrl1Run;
constexpr uint8_t kCtrl2Run   = QmcMag::kQmc6310Ctrl2Run;
constexpr float   kUtPerCount = QmcMag::kQmc6310UtPerCount;
constexpr const char* kPartName = "QMC6310";
// This part's data-ready bit is read before a sample is believed. Only on this
// part: the 6309 has shipped for months without it and a new gate on a
// verified board is a new way for a working compass to report nothing. Here it
// earns its transaction — the part powers up suspended, and its output
// registers before the first conversion are zeroes that would otherwise be
// taken for a reading, feed the hard-iron extremes a false centre, and pull
// every heading afterwards towards it.
constexpr bool kCheckDataReady = true;
#else
constexpr uint8_t kWantChipId = QmcMag::kChipIdQmc6309;
constexpr uint8_t kCtrl1Run   = QmcMag::kQmc6309Ctrl1Run;
constexpr uint8_t kCtrl2Run   = QmcMag::kQmc6309Ctrl2Run;
constexpr float   kUtPerCount = QmcMag::kQmc6309UtPerCount;
constexpr const char* kPartName = "QMC6309";
constexpr bool kCheckDataReady = false;
#endif

// Mode bits clear: the state the part is in when it is first powered, and the
// state the bench found it in before anything here had configured it — chip id
// 0x90 answering at 0x00 with both control registers at zero. Writing it back
// is therefore not a guess about a low-power mode, it is the mode this file's
// begin() takes the part out of.
//
// It clears more than the mode, which is why the resume writes two registers
// and not one: CTRL1 packs the mode, the oversampling and the output rate
// together — kCtrl1Run above is all three — so zero drops all of them. Only
// CTRL2's range survives a suspend, and applyMode() restores both anyway, in
// begin()'s order.
constexpr uint8_t kCtrl1Suspend = QmcMag::kCtrl1Suspend;

bool sUp = false;

// What the part is doing, and what it has been asked to do. The want is raised
// from whichever task noticed the screen move; the running flag is only ever
// written on the task that owns this part's bus access, at the top of poll(),
// and read back from the display task through running().
//
// Both cross tasks, so both are atomics and neither is a volatile bool. That
// distinction is not pedantry: volatile tells the compiler not to cache or
// fold the access and says nothing at all about it being indivisible or about
// what the other task may observe — it is a language feature for hardware
// registers, not a synchronisation primitive, and a plain bool shared between
// tasks is a data race whether or not it is spelled volatile. Relaxed, because
// each of these is a single flag with no companion state to be ordered against:
// the whole message is the value. Same shape and the same reasoning as
// Power's sScreenDark and Gps's sNavViewMs.
std::atomic<bool> sWantRunning{true};
std::atomic<bool> sRunning{false};

// The extremes each axis has reached, which is how the hard-iron offset is
// found: the readings from a board turned about lie on a sphere, and the centre
// of that sphere is the offset to subtract. Seeded inverted so the first
// reading replaces both ends.
float sMin[3] = {  1e9f,  1e9f,  1e9f };
float sMax[3] = { -1e9f, -1e9f, -1e9f };

// And kept across restarts, which is only right because the offset belongs to
// the board rather than to wherever the board is. That was worth checking
// rather than assuming: the M9 reads about 385 uT with nearly all of it
// perpendicular to the panel, which is equally the signature of the sounder
// magnet beside the sensor and of something flat underneath the device. Moved
// off a laptop it had been sitting on and onto a bare desk, the reading changed
// by less than a microtesla — so it is the board, and it travels with it.
//
// Had it gone the other way this would be wrong to keep: a stored calibration
// of one desk is worse than none, because the compass would look calibrated
// while being wrong everywhere else. Without keeping it, though, every power
// cycle discards the offsets and the heading is useless until somebody thinks
// to turn the device around — which is not something a user of a mesh node
// should have to know.
//
// The limit of doing it this way: these extremes only ever widen. If the board
// is later fixed inside something magnetic, the stored pair grows to span both
// worlds and never recovers, and there is no way to clear it short of erasing
// NVS. Worth a reset command the day a board needs one.
//
// Its own NVS namespace, so a key here cannot collide with a setting.
Preferences sStore;
bool        sStoreOpen = false;
// Written back at most this often: NVS is flash with a finite write budget, and
// the extremes move constantly during the turn that finds them. A minute's
// delay costs nothing — the values are already in RAM and in use — and turns
// thousands of writes into a handful.
constexpr uint32_t kSaveEveryMs = 60000;
uint32_t sLastSaveMs = 0;
bool     sDirty = false;

void loadOffsets() {
  if (!sStoreOpen) sStoreOpen = sStore.begin("compass", false);
  if (!sStoreOpen) return;
  // Both ends together or neither: half a calibration is worse than none,
  // because it centres the sphere on something no reading ever supported.
  if (sStore.getBytesLength("min") != sizeof(sMin) ||
      sStore.getBytesLength("max") != sizeof(sMax)) return;
  sStore.getBytes("min", sMin, sizeof(sMin));
  sStore.getBytes("max", sMax, sizeof(sMax));
  log_i("compass: hard-iron offsets recalled, centre %.0f,%.0f,%.0f uT",
        (sMin[0] + sMax[0]) * 0.5f, (sMin[1] + sMax[1]) * 0.5f,
        (sMin[2] + sMax[2]) * 0.5f);
}

// `force` skips the throttle and not the dirty test: it is for the one moment
// there will be no next chance — flush(), on the way into deep sleep or the
// charger's ship mode, after which the rail may not be there at all. It still
// writes nothing when nothing was found.
//
// Deliberately not the screen going dark. The dirty test is enough on a
// stationary node and is not on a carried one: twenty seconds lit at ten
// samples a second is two hundred readings through a changing field, and most
// such cycles find a new extreme on some axis. A handheld on the battery
// profile blanks every DISPLAY_SLEEP_BATTERY_MS — twenty seconds — so forcing
// there would write NVS three times faster than the minute this same file
// justifies with flash having a finite write budget. Losing up to a minute of
// extremes costs nearly nothing: the pair only ever widens, and the next turn
// finds again what was lost.
void saveOffsets(bool force = false) {
  if (!sStoreOpen || !sDirty) return;
  const uint32_t now = millis();
  if (!force && sLastSaveMs && now - sLastSaveMs < kSaveEveryMs) return;
  sLastSaveMs = now ? now : 1;
  sDirty = false;
  sStore.putBytes("min", sMin, sizeof(sMin));
  sStore.putBytes("max", sMax, sizeof(sMax));
}

inline TwoWire& bus() { return I2cReg::busFor(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ); }

} // namespace

namespace Compass {

bool present() { return sUp; }

void begin() {
  bool up = false;
  I2cReg::busFor(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ, &up);
  if (!up) { log_w("compass: bus would not start"); return; }

  const int id = I2cReg::read(bus(), COMPASS_ADDR, kChipId);
  if (id != (int)kWantChipId) {
    log_i("compass: no %s at 0x%02x (chip id read %d, wanted 0x%02x) — "
          "I2C on the console lists what answered", kPartName,
          (uint8_t)COMPASS_ADDR, id, (unsigned)kWantChipId);
    return;
  }

  // Reset before configuring. The part keeps running across a firmware restart
  // — nothing here powers it down — so without this it would be configured
  // from whatever state the previous run, or the board's factory firmware, left
  // it in rather than from a known one.
  //
  // Every write checked, and the part not counted as up unless all of them
  // landed. A magnetometer left suspended still acknowledges its address and
  // still answers a read of its output registers — with the zeros or the stale
  // values it was holding — so an unchecked configuration failure does not
  // produce a dead compass that reports itself dead. It produces a steady
  // heading that never moves, which is a far worse thing to hand a navigator.
  bool configured = I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, kSoftReset);
  delay(10);
  configured = configured && I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, 0x00);
  delay(10);
  configured = configured && I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, kCtrl2Run);
  configured = configured && I2cReg::write(bus(), COMPASS_ADDR, kCtrl1, kCtrl1Run);
  if (!configured) {
    log_w("compass: %s answered at 0x%02x but would not take its configuration "
          "— left off rather than reporting a heading that cannot change",
          kPartName, (uint8_t)COMPASS_ADDR);
    return;
  }
  delay(10);

  sUp = true;
  sRunning.store(true, std::memory_order_relaxed);   // configured into measure mode, above
  loadOffsets();
  log_i("compass: %s at 0x%02x, continuous; heading needs the board turned "
        "around once before the hard-iron offsets mean anything", kPartName,
        (uint8_t)COMPASS_ADDR);
}

// The last sample and when it was taken. Kept so that a caller asking for the
// heading gets the work the poller has already done, and so that two callers in
// the same pass do not each pay for a transaction.
//
// Written on the loop and read from the console task and the async web task,
// so the copy is taken under a lock: this is forty bytes of struct, and a
// reader that catches it half-written gets a heading from one sample with a
// calibration and a field strength from another. A spinlock rather than a
// mutex because the critical section is the copy itself and no reader may
// block on a bus — the same shape and the same reasoning as Power's history
// and Environment's reading on this very bus.
static portMUX_TYPE sLastMux = portMUX_INITIALIZER_UNLOCKED;
static Reading  sLast;

static void storeLast(const Reading& r) {
  taskENTER_CRITICAL(&sLastMux);
  sLast = r;
  taskEXIT_CRITICAL(&sLastMux);
}

static Reading loadLast() {
  Reading r;
  taskENTER_CRITICAL(&sLastMux);
  r = sLast;
  taskEXIT_CRITICAL(&sLastMux);
  return r;
}

// When a sample was last *attempted*, which is not when one last succeeded.
// The cadence has to run off this one: keyed on the success instead, a part
// that stops answering leaves the interval permanently expired and sample()
// runs on every pass of a loop that turns over about a thousand times a second
// — two I2C transactions each on the part that has its data-ready bit read,
// and fifty milliseconds each of TwoWire timeout on a bus held low. That is
// the same "the main loop becomes a 20 Hz loop" failure the mode-write retry
// above is rationed to avoid, and the data-ready gate below makes it far
// likelier: a part that is present, acknowledging and simply not converting
// hits it, where before it took a broken bus.
static uint32_t sLastTryMs = 0;

// Consecutive samples that produced nothing, and one line when they add up.
// A part that answers its address and never a reading is the failure this
// driver's begin() calls worse than a dead one, so it is said out loud once
// rather than left to be inferred from a heading that stopped moving.
static uint32_t sMisses = 0;
constexpr uint32_t kMissComplainAfter = 20;      // two seconds, at kPollMs

static void noteMiss() {
  if (sMisses < 0xFFFFFFFFu) sMisses++;
  if (sMisses == kMissComplainAfter)
    log_w("compass: the %s at 0x%02x has answered its address but produced no "
          "reading %u times running — no heading is being reported, which is "
          "what a caller now sees rather than the last one that worked",
          kPartName, (uint8_t)COMPASS_ADDR, (unsigned)kMissComplainAfter);
}

static Reading sample() {
  Reading r;
  if (!sUp) return r;

  // On the part that powers up suspended, ask whether there is a conversion to
  // read before reading one. Its output registers hold zeroes until the first
  // one lands, and a zero triple is not an absent reading — it is a reading at
  // the origin, which would widen the hard-iron extremes towards a centre the
  // field never had and pull every heading afterwards with it.
  if (kCheckDataReady) {
    const int st = I2cReg::read(bus(), COMPASS_ADDR, kStatus);
    if (st < 0 || (st & QmcMag::kStatusDataReady) == 0) { noteMiss(); return r; }
    // The overflow bit, while the byte is in hand. A clipped axis is the
    // failure QmcMag.h names for choosing an 8 G range: it does not saturate
    // visibly, it locks the heading, so it is worth a line rather than a
    // silently wrong bearing. Throttled on the same counter as a miss, since a
    // field strong enough to clip does not go away between samples.
    if ((st & QmcMag::kStatusOverflow) != 0 && (sMisses % kMissComplainAfter) == 0)
      log_w("compass: the %s reports an axis over its full scale — the heading "
            "will lock rather than swing; something magnetic is against the board",
            kPartName);
  }

  uint8_t d[6];
  if (!I2cReg::readN(bus(), COMPASS_ADDR, kDataX, d, sizeof(d))) { noteMiss(); return r; }

  float raw[3];
  for (int i = 0; i < 3; i++) {
    const int16_t v = QmcMag::axisCounts(d[i * 2], d[i * 2 + 1]);
    raw[i] = (float)v * kUtPerCount;
    r.magUt[i] = raw[i];
    if (raw[i] < sMin[i]) { sMin[i] = raw[i]; sDirty = true; }
    if (raw[i] > sMax[i]) { sMax[i] = raw[i]; sDirty = true; }
  }
  r.valid = true;
  r.fieldUt = MagHeading::magnitude(raw);

  // The board's own field removed, and how much of a turn the extremes have
  // seen. Both are arithmetic and both live in MagHeading.h, where a host can
  // check them against a vector worked out by hand — the calibration score in
  // particular, whose whole job is to say whether to believe the heading, and
  // which scores the worse of the two axes a bearing turns on rather than the
  // flattering best of three.
  float c[3];
  MagHeading::removeHardIron(raw, sMin, sMax, c);
  r.calibration = MagHeading::calibrationScore(sMin, sMax);

  // Gravity says where level went. Without it the horizontal plane is assumed
  // to be the board's own, which is true only while it is held flat — and on
  // the T-Beam Supreme it is the only case, because that unit's accelerometer
  // is faulty and never answers.
  float g[3];
  const MagHeading::Bearing b = (Imu::present() && Imu::accel(g))
                              ? MagHeading::tilted(c, g)
                              : MagHeading::flat(c);
  r.headingDeg = b.headingDeg;
  r.tiltDeg    = b.tiltDeg;
  r.levelled   = b.levelled;
  const uint32_t at = millis();
  r.atMs = at ? at : 1;
  sMisses = 0;
  storeLast(r);
  return r;
}

// Often enough that a board turned by hand cannot slip between samples — a
// comfortable turn takes a second or two, and twenty samples across it is
// plenty to find the extremes — and rare enough to be nothing on a bus this
// board barely uses.
constexpr uint32_t kPollMs = 100;

// A mode write that fails leaves the want standing, so it is tried again — and
// this is where that has to be bounded. applyMode() runs at the top of poll(),
// ahead of the sample cadence below, and poll() runs on every loop pass, so an
// unthrottled retry is a transaction per pass at about a kilohertz. On a bus
// held low each of those costs TwoWire::_timeOutMillis, 50 ms by default, and
// the main loop becomes a 20 Hz loop: Maintenance::poll, ConsoleServer::poll,
// the Reticulum inbox and admin passes and LocalLink::poll all slow by fifty
// times, silently, because the watchdog is still being fed. Rationed on the
// same gate the battery readers use — the attempt is what is rationed, so a
// write that fails still waits its turn — and said out loud once the failures
// have stopped looking like a hiccup.
static SampleGate sModeRetry(kPollMs);
static uint8_t    sModeFails = 0;
constexpr uint8_t kModeComplainAfter = 10;   // a second of them, at kPollMs

static void noteModeFailure(void) {
  if (sModeFails < 255) sModeFails++;
  if (sModeFails == kModeComplainAfter)
    log_w("compass: the %s at 0x%02x has refused %u mode writes in a row — the "
          "part is left as it was and the retry stays on the %u ms cadence",
          kPartName, (uint8_t)COMPASS_ADDR, (unsigned)kModeComplainAfter,
          (unsigned)kPollMs);
}

// The wanted mode, written here and nowhere else — on the task that owns this
// part, which is what keeps the screen's verdict off a bus it does not own.
static void applyMode() {
  const bool want = sWantRunning.load(std::memory_order_relaxed);
  if (want == sRunning.load(std::memory_order_relaxed)) return;
  if (!sModeRetry.due(millis())) return;
  if (want) {
    // Both control registers, in begin()'s order, and both of them are needed:
    // the suspend write took CTRL1's oversampling and output rate down with
    // its mode bits, since all three share that register (kCtrl1Suspend). Only
    // CTRL2's range should have survived, and it is restated anyway — one
    // transaction on a wake, and no question left about what a suspended part
    // remembers.
    if (!I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, kCtrl2Run)) { noteModeFailure(); return; }
    if (!I2cReg::write(bus(), COMPASS_ADDR, kCtrl1, kCtrl1Run)) { noteModeFailure(); return; }
    // Nothing was measured while it slept, so the held sample is from before
    // the screen went dark and the output registers still hold that conversion.
    // Dropped, and the next sample left a poll interval away, which is twenty
    // conversions at the rate above: a reader in that window is told there is
    // no heading yet, which is true, rather than one from a minute ago.
    storeLast(Reading{});
  } else {
    if (!I2cReg::write(bus(), COMPASS_ADDR, kCtrl1, kCtrl1Suspend)) { noteModeFailure(); return; }
    storeLast(Reading{});
    // Nothing is flushed here. The screen goes dark every twenty seconds on a
    // handheld and the offsets are worth a minute of waiting, not a write per
    // blank — see saveOffsets() above. The moment there really is no next
    // chance is the node being switched off, and that comes through flush().
  }
  sModeFails = 0;
  sRunning.store(want, std::memory_order_relaxed);
}

void setRunning(bool run) { sWantRunning.store(run, std::memory_order_relaxed); }

void flush() { saveOffsets(true); }

bool running() { return sRunning.load(std::memory_order_relaxed); }

void poll() {
  if (!sUp) return;
  applyMode();                           // the screen's verdict, on this task
  if (!sRunning.load(std::memory_order_relaxed)) return;   // suspended: nothing to sample
  const uint32_t now = millis();
  // Rationed on the attempt, not on the success — see sLastTryMs.
  if (sLastTryMs && now - sLastTryMs < kPollMs) return;
  sLastTryMs = now ? now : 1;
  sample();
  saveOffsets();
}

// How long a reading may go unrefreshed before it stops being an answer. Five
// intervals: long enough that a loop pass held up by something else does not
// make the heading flicker, short enough that nobody is ever shown a bearing
// from a part that has gone quiet.
constexpr uint32_t kStaleMs = 5 * kPollMs;

Reading read() {
  if (!sUp || !sRunning.load(std::memory_order_relaxed)) return Reading{};
  // The poller's copy, and never a sample of its own — which is a change, and
  // the reason is the bus rather than this part. This used to take a reading
  // when the last one was stale, on whichever task asked: the console, and now
  // the status API. I2cReg drains a read outside the bus lock (issue 33), so
  // two tasks *reading* one bus can each end up with some of the other's
  // bytes, and on the T-Beam Supreme this part shares a bus with a BME280 that
  // the main loop reads every thirty seconds. One reader is what makes that
  // bus safe, and it is the loop.
  //
  // And the copy is only handed over while it is current. An earlier version
  // of this returned it unconditionally, on the reasoning that a part which
  // stops answering "stops updating sLast" — which is true and was exactly
  // backwards: not updating it is what leaves the last good heading standing.
  // sample() returns early on every failure path, so a part that resets into
  // its suspend state, or a bus that stops carrying, would have left a
  // plausible bearing in place for as long as the node ran, with present,
  // running and valid all reporting health. That is the outcome begin() calls
  // worse than a dead compass, arrived at by way of a comment that argued for
  // it.
  const Reading r = loadLast();
  if (!r.valid) return Reading{};
  if (millis() - r.atMs > kStaleMs) return Reading{};
  return r;
}

uint32_t ageS(const Reading& r) {
  return r.valid ? (uint32_t)((millis() - r.atMs) / 1000) : 0;
}

} // namespace Compass

#endif // HAS_COMPASS
