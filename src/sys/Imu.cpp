// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Imu.cpp — see Imu.h
//
//  The DA217 numbers come from the MiraMEMS datasheet. The vendor's spec sheet
//  names the part; an earlier draft guessed a QMI8658 off the factory image's
//  multi-driver probe strings, which is the lesson: a probe list names what the
//  firmware could drive, not what the board has.
//
//  The QMI8658 numbers come from QST's own datasheet (rev 0.9), and this time
//  the part was read off the bus before a line was written — WHO_AM_I 0x05 at
//  0x6b on the M9, by `I2C 0x6b` on the console. Which also caught the trap
//  below: this part does not auto-increment its register pointer by default, so
//  a burst read of the output block returns the same byte six times. Written
//  from the register map alone, that reads as a dead part on a good bus.
// ============================================================================
#include "Imu.h"

#if HAS_IMU

#include <Arduino.h>
#include <Wire.h>
#include "I2cReg.h"
#include "Lock.h"
#include "SampleGate.h"
#include <atomic>
#if IMU_TRANSPORT == IMU_TRANSPORT_SPI
  #include <SPI.h>
  #include "SpiBus.h"
#endif

namespace {

// The I2C address, once something has answered at it. Only the I2C transport
// reads this — the DA217 is probed across two addresses, so it cannot be a
// constant — and on SPI there is no address at all, which is why presence is
// the separate flag below rather than "sAddr is nonzero" as it used to be.
uint8_t sAddr = 0;
bool sPresent = false;                   // answered, and took its configuration

// What the part is doing, and what it has been asked to do. The mode write is
// deferred to poll() and every register access here is taken under one lock —
// but not for the reason an earlier draft of this comment gave. The Arduino
// bus object protects its own transaction: CONFIG_DISABLE_HAL_LOCKS is unset
// on both chips, so beginTransmission takes the bus lock, endTransmission(true)
// gives it back, and endTransmission(false) deliberately holds it through the
// requestFrom that follows (Wire.cpp). Two tasks cannot interleave their
// address and register bytes, whatever they are doing.
//
// What is not protected is the receive buffer. I2cReg::read and readN drain it
// with bus.read() *after* requestFrom has released the lock, so two tasks
// *reading* one bus can each end up with some of the other's bytes. Reads are
// the hazard, and this part is read from two tasks across the fleet: the
// display task asks which way up the panel is held, the main loop asks where
// level went for the compass's tilt correction. The lock below orders this
// driver against itself, which is all a driver can do for itself — on the V4
// the same bus also carries the charger, which Power::battery() reads from the
// loop, from the async web task and from the Reticulum task, and ordering this
// part's reads against those is a bus-wide question no one driver can answer.
//
// What the lock does not cover is the pair of flags below, and it never did:
// running() answers the display task without it, and poll()'s first line reads
// both to find out whether there is anything to do at all. So they are atomics
// rather than a volatile bool and a plain one — volatile orders nothing and
// promises nothing about what another task observes; it is for hardware
// registers, not for sharing state between tasks. Relaxed, because each is a
// single flag whose whole message is its value, with no companion state to be
// ordered against: the register write that the running flag reports on is
// ordered by the lock, as it was before. Same shape as Power's sScreenDark and
// Gps's sNavViewMs.
SemaphoreHandle_t sLock = nullptr;
std::atomic<bool> sWantRunning{true};    // raised from any task
std::atomic<bool> sRunning{false};       // moved only once the write landed

// The three calls the rest of this file is written in terms of, and the only
// thing that differs between a part on I2C and the same part on SPI. Every
// register number, the chip id, the configuration and the sign work below are
// the part's and are shared.
#if IMU_TRANSPORT == IMU_TRANSPORT_SPI

// 4-wire SPI: the address byte carries the direction in its top bit, set to
// read and clear to write, and the part auto-increments across a burst once
// CTRL1's ADDR_AI bit is set — which begin() does on either transport.
//
// The object comes from SpiBus rather than being constructed here, because on
// the board that wires the accelerometer this way the card is on the same host
// and two SPIClass objects on one host re-initialise the peripheral under each
// other (SpiBus.h). That shared object is also what makes this safe against
// the card without a lock of this driver's own: beginTransaction takes the
// core's per-bus mutex and holds it across the transfer, so none of the I2C
// read-drain hazard described above applies here.
//
// 1 MHz because these are transfers of a few bytes and there is nothing to
// gain by hurrying them; the card negotiates its own speed in its own
// transactions.
constexpr uint32_t kSpiHz = 1000000;

// What the log calls the part's place. An address on a bus that has them, the
// select pin on one that does not — because "at 0x00" on an SPI board would be
// a fact about nothing.
#define IMU_AT_FMT "on SPI (select GPIO %d)"
#define IMU_AT_ARG PIN_IMU_CS
// Where to look next, and it is not the I2C scanner: nothing on this bus has an
// address, so the console's scan cannot see this part however hard it looks.
// What is worth checking is named instead — and the card is the control,
// because it is on these same three wires, so a card that mounts says the bus,
// the pins and the shared host are all good and leaves the select and the part.
#define IMU_HINT "the console's I2C scan cannot see an SPI part — check the " \
                 "select and the part; if the card on these same wires mounts, " \
                 "the bus and the pins are not the problem"

inline SPIClass& spi() {
  return SpiBus::get(IMU_SPI_BUS, PIN_IMU_SCK, PIN_IMU_MISO, PIN_IMU_MOSI);
}

bool readRegs(uint8_t reg, uint8_t* out, size_t n) {
  SPIClass& b = spi();
  b.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_IMU_CS, LOW);
  b.transfer((uint8_t)(reg | 0x80));
  for (size_t i = 0; i < n; i++) out[i] = b.transfer(0x00);
  digitalWrite(PIN_IMU_CS, HIGH);
  b.endTransaction();
  return true;                           // SPI has no acknowledgement to fail
}

int readReg(uint8_t reg) {
  uint8_t v = 0;
  if (!readRegs(reg, &v, 1)) return -1;
  return v;
}

bool writeReg(uint8_t reg, uint8_t val) {
  SPIClass& b = spi();
  b.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_IMU_CS, LOW);
  b.transfer((uint8_t)(reg & 0x7F));
  b.transfer(val);
  digitalWrite(PIN_IMU_CS, HIGH);
  b.endTransaction();
  // Nothing on this bus says a write landed, so the caller cannot tell a
  // refused configuration from a taken one here the way the I2C path can.
  // begin() covers that itself: it reads the registers back.
  return true;
}

#else

#define IMU_AT_FMT "at 0x%02x"
#define IMU_AT_ARG sAddr
#define IMU_HINT "I2C on the console lists what answers there"

inline TwoWire& bus() { return I2cReg::busFor(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ); }

int  readReg(uint8_t reg)                          { return I2cReg::read(bus(), sAddr, reg); }
bool readRegs(uint8_t reg, uint8_t* out, size_t n) { return I2cReg::readN(bus(), sAddr, reg, out, n); }
bool writeReg(uint8_t reg, uint8_t val)            { return I2cReg::write(bus(), sAddr, reg, val); }

#endif

#if IMU_KIND == IMU_KIND_QMI8658
// QST QMI8658, datasheet rev 0.9.
constexpr uint8_t kWhoAmI   = 0x00;      // 0x05 on every part in this family
constexpr uint8_t kCtrl1    = 0x02;      // bit 6 ADDR_AI, bit 5 BE
constexpr uint8_t kCtrl2    = 0x03;      // bits 6:4 full scale, bits 3:0 rate
constexpr uint8_t kCtrl7    = 0x08;      // bit 0 accelerometer on
// The one bit that starts and stops the accelerometer, named at both values
// because both are now written: clearing it is the part's own low-power state,
// and it is the same bit begin() sets, not a mode read off a datasheet nothing
// here has checked.
constexpr uint8_t kCtrl7On  = 0x01;
constexpr uint8_t kCtrl7Off = 0x00;
constexpr uint8_t kAccelX   = 0x35;      // AX_L; six bytes to AZ_H
// ±2 g at 62.5 Hz. The smallest range because this is asked about gravity and
// nothing faster, and the smallest range is the finest resolution: 16384 counts
// per g, from the datasheet's sensitivity table.
constexpr uint8_t kCtrl2Value = 0x07;
constexpr float   kCountsPerG = 16384.0f;
#else
// MiraMEMS DA217.
constexpr uint8_t kChipId     = 0x01;    // 0x13
constexpr uint8_t kAccelX     = 0x02;
// Power mode. Both values come from the same place as the enable value this
// driver already used — the kernel's da280 driver, which names 0x1e and 0x9e
// as the pair — so the suspend half is as documented as the run half, and the
// difference between them is the one bit the family reserves for it.
constexpr uint8_t kMode        = 0x11;
constexpr uint8_t kModeNormal  = 0x1e;
constexpr uint8_t kModeSuspend = 0x9e;
// 14-bit, left-justified, at the ±2 g the part is put in below. Unverified on
// hardware: the only board carrying this part has no magnetometer, so nothing
// asks it for a number rather than a ratio. facing() proves the axes; this
// proves nothing until a board needs it.
constexpr float   kCountsPerG = 4096.0f;
#endif

// The three axes as signed counts, however this part packs them. False when the
// read failed — which on the QMI8658 includes the case that matters, since a
// part whose auto-increment was never enabled returns one register six times.
bool rawAxes(int16_t& x, int16_t& y, int16_t& z) {
  if (!sPresent) return false;
  Sys::Lock held(sLock);
  // A suspended part still answers its address and still returns the last
  // conversion it made, which is the same trap begin() guards against: the
  // reading would look like a board that has not moved since the screen went
  // dark. Nothing is the honest answer, and both callers already have one.
  if (!sRunning.load(std::memory_order_relaxed)) return false;
  uint8_t raw[6];
  if (!readRegs(kAccelX, raw, sizeof(raw))) return false;
#if IMU_KIND == IMU_KIND_QMI8658
  x = (int16_t)(raw[0] | (raw[1] << 8));
  y = (int16_t)(raw[2] | (raw[3] << 8));
  z = (int16_t)(raw[4] | (raw[5] << 8));
#else
  // Left-justified little-endian pairs; the shift keeps the sign.
  x = (int16_t)((int16_t)(raw[0] | (raw[1] << 8)) >> 4);
  y = (int16_t)((int16_t)(raw[2] | (raw[3] << 8)) >> 4);
  z = (int16_t)((int16_t)(raw[4] | (raw[5] << 8)) >> 4);
#endif
  // Into the panel's frame, which is the only frame the callers think in. Both
  // of them — the display's rotation and the compass's tilt correction — ask
  // "which way is down relative to the screen", so the mounting has to be
  // undone here rather than in each of them.
#if IMU_INVERT_X
  x = (int16_t)-x;
#endif
#if IMU_INVERT_Y
  y = (int16_t)-y;
#endif
#if IMU_INVERT_Z
  z = (int16_t)-z;
#endif
  return true;
}

} // namespace

namespace Imu {

void begin() {
  // Before the first register access, and before any task can ask for one.
  // A null handle is not fatal — Sys::Lock treats it as no lock — so a board
  // that could not allocate one behaves exactly as this driver did before.
  if (!sLock) sLock = xSemaphoreCreateMutex();
#if IMU_TRANSPORT == IMU_TRANSPORT_SPI
  // This driver's own select, raised before the first transfer, for the same
  // reason every other SPI driver here raises its own: a driver that works
  // only because something else prepared its pin breaks when the order
  // changes. BoardInit idles this one *and the card's* before either driver
  // exists, which is what makes the probe below safe — on this board it is
  // this begin() that runs first, so the card's select is the floating one to
  // worry about.
  pinMode(PIN_IMU_CS, OUTPUT);
  digitalWrite(PIN_IMU_CS, HIGH);
#endif
#if IMU_KIND == IMU_KIND_QMI8658
  // One address, strapped by SDO and not by anything this board can change.
  // Nothing to strap on SPI, where the select does the addressing.
  sAddr = IMU_ADDR;
  const int who = readReg(kWhoAmI);
  if (who != 0x05) {
    // The value, not just the disappointment. 0xff or 0x00 from every register
    // is a part that is not there or not selected; anything else is a part
    // that is talking and being misread, and those want different work.
    log_i("imu: no QMI8658 answers " IMU_AT_FMT " — WHO_AM_I read 0x%02x, wanted 0x05; "
          IMU_HINT, IMU_AT_ARG, (unsigned)(who & 0xFF));
    sAddr = 0;
    return;
  }
  // Auto-increment first, and it is not optional: without it every register in
  // a burst read comes back as the first one, so the output block reads as six
  // copies of the X low byte. BE left clear, so a pair is low byte then high.
  //
  // Checked, all three. This part answers its address whatever state it is in,
  // so a configuration that did not land leaves an accelerometer that reads —
  // the same six bytes for ever, because auto-increment is the very write most
  // worth losing. Six copies of one byte is what an unconfigured part looks
  // like, and it is exactly what the bus scan saw before any of this existed.
  bool configured = writeReg(kCtrl1, 0x40);
  configured = configured && writeReg(kCtrl2, kCtrl2Value);
  configured = configured && writeReg(kCtrl7, kCtrl7On);  // accelerometer only; the gyro
                                                         // costs milliamps and answers
                                                         // nothing asked here
  // And then, on SPI only, the part is asked whether it took the one write
  // worth losing. On I2C a refused write is a NAK and the flag above already
  // has it; on SPI nothing acknowledges anything, so a write that went nowhere
  // looks exactly like one that landed.
  //
  // Only on SPI, deliberately. An earlier version ran this on both buses and
  // called it "the same standard of proof", which was wrong twice: it adds
  // nothing on the bus that already reports refusals, and it adds a new way
  // for a verified board to decide its accelerometer is absent. And note the
  // read has to be checked for failure before its bits are: readReg returns
  // -1 on a NAK, and -1 & 0x40 is 0x40, so the obvious spelling of this test
  // passes on exactly the failure it exists to catch.
#if IMU_TRANSPORT == IMU_TRANSPORT_SPI
  const int ctrl1 = readReg(kCtrl1);
  configured = configured && ctrl1 >= 0 && (ctrl1 & 0x40) != 0;
#endif
  if (!configured) {
    log_w("imu: QMI8658 answered " IMU_AT_FMT " but would not take its configuration "
          "— left off rather than reading the same six bytes for ever", IMU_AT_ARG);
    sAddr = 0;
    return;
  }
  sPresent = true;
  sRunning.store(true, std::memory_order_relaxed);
  log_i("imu: QMI8658 " IMU_AT_FMT ", accelerometer running at +/-2 g", IMU_AT_ARG);
#else
  for (uint8_t addr : { (uint8_t)0x26, (uint8_t)0x27 }) {
    sAddr = addr;
    if (readReg(kChipId) == 0x13) break; // chip id says DA217
    sAddr = 0;
  }
  if (!sAddr) { log_i("imu: no DA217 answers"); return; }
  // 0x1e is the documented enable value for this family (the kernel's da280
  // driver uses exactly it); the first draft wrote 0x00, whose bandwidth code
  // the datasheet reserves — it ran, but on the datasheet's silence.
  //
  // Checked, like the QMI8658's above and for the same reason: this part
  // answers its chip id whatever state it is in, so an unchecked configuration
  // leaves a part recorded as running that is converting in some other mode,
  // or not at all. The panel would follow a hand that is not there. Left off
  // instead, where present() says so and the log says why.
  bool configured = writeReg(kMode, kModeNormal);   // normal power, documented bandwidth
  configured = configured && writeReg(0x0F, 0x00);  // ±2g — orientation needs no more
  if (!configured) {
    log_w("imu: DA217 answered at 0x%02x but would not take its configuration "
          "— left off rather than recorded as running", sAddr);
    sAddr = 0;
    return;
  }
  sPresent = true;
  sRunning.store(true, std::memory_order_relaxed);
  log_i("imu: DA217 at 0x%02x, accelerometer running", sAddr);
#endif
}

bool present() { return sPresent; }

bool running() { return sRunning.load(std::memory_order_relaxed); }

void setRunning(bool run) { sWantRunning.store(run, std::memory_order_relaxed); }

// A failed mode write leaves the want standing, so poll() tries again on the
// next pass — and the next pass is a millisecond away. Rationed on the same
// gate the battery readers use, and for the reason the magnetometer's is
// (Compass.cpp): a bus held low costs TwoWire::_timeOutMillis per transaction,
// 50 ms by default, and a retry per loop pass turns the whole main loop into a
// 20 Hz loop while the watchdog goes on being fed. Touched only from poll(),
// which is the main loop's alone, so these need no lock of their own.
constexpr uint32_t kModeRetryMs       = 100;
constexpr uint8_t  kModeComplainAfter = 10;   // a second of them, at that rate
static SampleGate  sModeRetry(kModeRetryMs);
static uint8_t     sModeFails = 0;

// The last axes this part gave up, and when. Every reader outside the main
// loop gets this copy rather than a transaction of its own, which is the same
// rule the magnetometer and the environmental sensor follow and for the same
// reason: I2cReg drains a read *after* requestFrom has released the bus lock
// (issue 33), so two tasks reading one bus can each take some of the other's
// bytes. This part has three readers besides the loop — the console, the
// status API and, where a panel turns itself, the display task — and on the
// board where it sits on I2C that bus also carries the magnetometer the loop
// reads ten times a second. One reader is what makes such a bus safe, and it
// is the loop.
//
// It costs nothing in traffic. The compass already asked this part for gravity
// on every one of its samples, so the read happens at the same rate it always
// did; what changes is that the other three readers now cost nothing at all.
// The cadence is whatever the board's fastest reader wanted, so that moving
// the read into this loop costs no board more bus traffic than it already
// paid:
//
//   * where there is a magnetometer, the compass asked this part for gravity on
//     every one of its own samples — ten a second — so that is the rate, and
//     it is exactly what these boards did before;
//   * everywhere else the only consumer is a panel that turns itself, and
//     Display.cpp asks once a second and wants two agreeing answers a second
//     apart before it turns anything. A read per second is what that board was
//     doing on demand, so it is what it does now.
//
// The console and the status API are then free on both, where before each of
// their reads was a transaction on a bus they do not own.
#if HAS_COMPASS
constexpr uint32_t kSampleMs = 100;
#else
constexpr uint32_t kSampleMs = 1000;
#endif
static SampleGate  sSampleGate(kSampleMs);
static portMUX_TYPE sAxesMux = portMUX_INITIALIZER_UNLOCKED;
static bool     sAxesValid = false;
static int16_t  sAxes[3]   = {0, 0, 0};
static uint32_t sAxesAtMs  = 0;

// Five intervals, the magnetometer's bound and for the same reason: long
// enough that a busy loop pass does not make a reading flicker, short enough
// that nobody is handed axes from a part that has stopped answering. A board
// that has not moved since the node booted must not be indistinguishable from
// a part that fell off the bus. Half a second where the compass sets the rate,
// five where the panel does — which is still well inside the second the
// rotation logic waits out before it turns anything.
constexpr uint32_t kAxesStaleMs = 5 * kSampleMs;

static void storeAxes(bool valid, int16_t x, int16_t y, int16_t z) {
  const uint32_t at = millis();
  taskENTER_CRITICAL(&sAxesMux);
  sAxesValid = valid;
  sAxes[0] = x; sAxes[1] = y; sAxes[2] = z;
  sAxesAtMs = at ? at : 1;
  taskEXIT_CRITICAL(&sAxesMux);
}

// True with the axes filled in, false with them untouched. The freshness test
// is inside the lock with the copy, so a reader cannot pair one sample's axes
// with another's age.
static bool loadAxes(int16_t& x, int16_t& y, int16_t& z) {
  const uint32_t now = millis();
  bool ok = false;
  taskENTER_CRITICAL(&sAxesMux);
  if (sAxesValid && now - sAxesAtMs <= kAxesStaleMs) {
    x = sAxes[0]; y = sAxes[1]; z = sAxes[2];
    ok = true;
  }
  taskEXIT_CRITICAL(&sAxesMux);
  return ok;
}

// The part's own reading, taken here and nowhere else. Called from poll()
// below, which is the main loop's alone.
static void sampleAxes() {
  int16_t x = 0, y = 0, z = 0;
  if (rawAxes(x, y, z)) storeAxes(true, x, y, z);
  // A failed read is deliberately not stored as invalid: the freshness bound
  // in loadAxes() already turns a part that has stopped answering into no
  // reading within half a second, and clearing it here would make one dropped
  // transaction look the same as a part that fell off the bus.
}

void poll() {
  if (!sPresent) return;

  // The reading first, on its own cadence and only while the part is meant to
  // be converting. rawAxes() itself refuses while suspended — a suspended part
  // answers its address and returns the conversion it made before it stopped,
  // which would read as a board that has not moved since the screen went dark.
  if (sRunning.load(std::memory_order_relaxed) && sSampleGate.due(millis()))
    sampleAxes();

  // Read without the lock on purpose: two atomic flags with one writer each
  // and no companion state, so the worst a stale view can do is defer the
  // change to the next pass a millisecond later. Taking a mutex a thousand
  // times a second to discover there is nothing to do is the cost this guard
  // exists to avoid.
  if (sWantRunning.load(std::memory_order_relaxed) ==
      sRunning.load(std::memory_order_relaxed)) return;
  if (!sModeRetry.due(millis())) return;
  Sys::Lock held(sLock);
  const bool want = sWantRunning.load(std::memory_order_relaxed);
  // Settled while we waited for the lock.
  if (want == sRunning.load(std::memory_order_relaxed)) return;
#if IMU_KIND == IMU_KIND_QMI8658
  const uint8_t modeReg = kCtrl7, modeVal = want ? kCtrl7On : kCtrl7Off;
#else
  const uint8_t modeReg = kMode, modeVal = want ? kModeNormal : kModeSuspend;
#endif
  bool wrote = writeReg(modeReg, modeVal);
#if IMU_TRANSPORT == IMU_TRANSPORT_SPI
  // And on SPI, read it back, because nothing else can say the write landed:
  // writeReg() returns true unconditionally there (no acknowledgement exists
  // on this bus), so without this the flag below would flip on a write that
  // went nowhere. A part recorded as suspended that is still converting is
  // what prepareForSleep() waits for and would then stop waiting for — deep
  // sleep holds the pins as they stand, so the part would convert through a
  // menu item named "off". begin() already reads CTRL1 back for the same
  // reason and this is the same proof applied to the same bus.
  //
  // SPI only, deliberately: the I2C path has a NAK and already reports a
  // refusal, and a new gate on a verified board is a new way for a working
  // accelerometer to be declared stuck.
  if (wrote) {
    const int back = readReg(modeReg);
    wrote = back >= 0 && (uint8_t)back == modeVal;
  }
#endif
  if (!wrote) {
    if (sModeFails < 255) sModeFails++;
    if (sModeFails == kModeComplainAfter)
      log_w("imu: the accelerometer " IMU_AT_FMT " has refused %u mode writes in a row — "
            "the part is left as it was and the retry stays on the %u ms cadence",
            IMU_AT_ARG, (unsigned)kModeComplainAfter, (unsigned)kModeRetryMs);
    return;
  }
  // Only once the write landed. A part recorded as suspended that is still
  // converting costs power silently; one recorded as running that is not
  // reads as a board that never moves, and this driver already refuses to
  // ship that.
  sModeFails = 0;
  sRunning.store(want, std::memory_order_relaxed);
  // A suspended part keeps answering its address and keeps returning the last
  // conversion it made, so the copy it left behind has to go with it: held, it
  // would read as a board that has not moved since the screen went dark, which
  // is the one answer this driver refuses to give. The freshness bound would
  // catch it half a second later anyway; this catches it now.
  if (!want) storeAxes(false, 0, 0, 0);
}

bool accel(float g[3]) {
  int16_t x, y, z;
  if (!loadAxes(x, y, z)) return false;
  g[0] = (float)x / kCountsPerG;
  g[1] = (float)y / kCountsPerG;
  g[2] = (float)z / kCountsPerG;
  return true;
}

Facing facing() {
  int16_t ax, ay, az;
  if (!loadAxes(ax, ay, az)) return Facing::Unknown;
  // Only the axes' ratios matter here, so the exact scale is deliberately not
  // chased — every axis is scaled alike whichever part answered.
  const int32_t mx = ax < 0 ? -ax : ax, my = ay < 0 ? -ay : ay, mz = az < 0 ? -az : az;
  // Lying flat, gravity is all Z and the panel's rotation is nobody's guess.
  if (mz > mx * 2 && mz > my * 2) return Facing::Flat;
  if (my >= mx) return ay < 0 ? Facing::Up0 : Facing::Up180;
  return ax < 0 ? Facing::Up90 : Facing::Up270;
}

} // namespace Imu

#endif // HAS_IMU
