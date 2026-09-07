// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Gps.cpp — see Gps.h
// ============================================================================
#include "Gps.h"

#if HAS_GPS

#include "GnssDutyPolicy.h"
#include "Pmu.h"
#include "Power.h"
#include "Rtc.h"
#include "Settings.h"
#include "UbxFrame.h"
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <time.h>
#include <atomic>
#include "Diag.h"
#include "Lock.h"
#include "Watchdog.h"

namespace {

// The longest sentence we care about is well under this. Anything longer is
// discarded rather than truncated, so a partial line can never be parsed.
const uint16_t NMEA_MAX      = 100;
// The receiver's clock is far better than ours, but re-adopting it every
// sentence would be pointless churn.
const uint32_t SYNC_INTERVAL = 3600000;
// How often a port that did not come back is re-opened. The failure this
// serves is a UART driver that could not be installed (openPort below), which
// is a memory condition rather than a wiring one, so retrying costs a driver
// install every five seconds until it succeeds — not once per pass, which
// would be a hundred a second and a log line with each.
const uint32_t PORT_RETRY_MS = 5000;

HardwareSerial   sSerial(1);
SemaphoreHandle_t sLock = nullptr;
// Set by the task itself before its first pass; setEnabled(true) uses it to
// wake a task parked in the disabled branch's notify-wait immediately,
// rather than leaving it to find out on the next timeout.
TaskHandle_t     sTaskHandle = nullptr;
Gps::Fix         sFix;
char             sLine[NMEA_MAX];
uint16_t         sLen = 0;
bool             sOverflow = false;
uint32_t         sLastFixMs = 0, sLastSentenceMs = 0, sLastSyncMs = 0;
uint16_t         sYear = 0;
uint8_t          sMonth = 0, sDay = 0, sHour = 0, sMinute = 0, sSecond = 0;

// --- the duty cycle (GnssDutyPolicy.h) ---------------------------------------
GnssDutyPolicy   sDuty;
// What the hardware has actually been told, as opposed to what the policy last
// answered: the two differ for exactly one pass, which is where the edge is.
bool             sResting = false;
// When the current tracking window began. Two things are measured from it — a
// fix only counts towards the next rest once it has been re-asserted since the
// receiver was allowed to look again, and the reported fix is given the same
// grace after a rest that it already gets after a lost signal.
uint32_t         sTrackFromMs = 0;
// When a screen whose purpose is showing position last painted. Written on the
// display task and read on this one, so it is an atomic rather than a plain
// word that happens to be aligned: relaxed, because nothing is published
// alongside it — the value *is* the message — and stating that is what stops
// a link-time optimiser hoisting the read out of the reader's loop.
std::atomic<uint32_t> sNavViewMs{0};
// When the port was last opened, or last tried. See openPort().
uint32_t         sLastPortTryMs = 0;
#if GPS_NAP == GPS_NAP_PMREQ
uint32_t         sLastNudgeMs = 0;
// Whether this run has actually asked the receiver to stop — which on this
// board means a power-management request has gone out. It gates the nudge:
// with nothing ever asked there is nothing to recover from, and a board whose
// receiver socket is empty runs those pins out to an expansion connector.
bool             sEverAsked = false;
// The interval the current rest is worth, handed to the receiver as its own
// backstop timer — see the message's own comment for why it is not zero.
uint32_t         sRestMs = 0;
#endif

// A sentence is "$" + payload + "*" + two hex digits of XOR over the payload.
bool checksumOk(const char* s, uint16_t len) {
  if (len < 5 || s[0] != '$') return false;
  int star = -1;
  for (int i = len - 3; i >= 1; i--) if (s[i] == '*') { star = i; break; }
  if (star < 1) return false;
  uint8_t sum = 0;
  for (int i = 1; i < star; i++) sum ^= (uint8_t)s[i];
  return sum == (uint8_t)strtol(s + star + 1, nullptr, 16);
}

// Copies comma-separated field `idx`. False for empty fields, which NMEA uses
// liberally whenever a value is not yet available.
bool field(const char* s, uint8_t idx, char* out, uint8_t outlen) {
  uint8_t f = 0, o = 0;
  for (uint16_t i = 0; s[i] && s[i] != '*'; i++) {
    if (s[i] == ',') {
      if (f == idx) { out[o] = 0; return o > 0; }
      f++; o = 0; continue;
    }
    if (f == idx && o < outlen - 1) out[o++] = s[i];
  }
  if (f == idx) { out[o] = 0; return o > 0; }
  return false;
}

// NMEA gives coordinates as ddmm.mmmm, not decimal degrees.
double toDegrees(const char* value, const char* hemisphere) {
  double raw = atof(value);
  int deg = (int)(raw / 100.0);
  double result = deg + (raw - deg * 100.0) / 60.0;
  if (hemisphere[0] == 'S' || hemisphere[0] == 'W') result = -result;
  return result;
}

// The receiver's time is authoritative. TZ is pinned to UTC in begin(), which
// makes mktime() a UTC conversion — which is what NMEA gives us.
void syncClock() {
  if (sYear < 2020) return;                      // not a real date yet
  if (sFix.clockSet && millis() - sLastSyncMs < SYNC_INTERVAL) return;

  struct tm t = {};
  t.tm_year = sYear - 1900; t.tm_mon = sMonth - 1; t.tm_mday = sDay;
  t.tm_hour = sHour; t.tm_min = sMinute; t.tm_sec = sSecond;
  time_t epoch = mktime(&t);
  if (epoch <= 0) return;

  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  // And into the board's own clock, if it has one. This is the direction that
  // matters: the receiver is the only source of true time here, and a board
  // that can hold it needs telling once for every boot afterwards to start
  // right. Written on the same schedule this syncs on rather than only the
  // first time, so a part that was set and then lost the time gets it back
  // without waiting for a restart.
  Rtc::write(epoch);
  if (!sFix.clockSet)
    log_i("clock set from GNSS: %04u-%02u-%02u %02u:%02u:%02u UTC",
          sYear, sMonth, sDay, sHour, sMinute, sSecond);
  sFix.clockSet = true;
  sLastSyncMs = millis();
}

// Recommended Minimum: position, speed and the date.
void parseRmc(const char* s) {
  char f[16], h[4];
  const bool valid = field(s, 2, f, sizeof(f)) && f[0] == 'A';
  if (valid) {
    char lat[16], lon[16];
    if (field(s, 3, lat, sizeof(lat)) && field(s, 4, h, sizeof(h))) sFix.latitude  = toDegrees(lat, h);
    if (field(s, 5, lon, sizeof(lon)) && field(s, 6, h, sizeof(h))) sFix.longitude = toDegrees(lon, h);
    if (field(s, 7, f, sizeof(f))) sFix.speedKmh = atof(f) * 1.852f;   // knots
    sFix.valid = true;
    sLastFixMs = millis();
  } else {
    sFix.valid = false;
  }

  char tf[16], df[16];                            // hhmmss.ss and ddmmyy
  if (field(s, 1, tf, sizeof(tf)) && strlen(tf) >= 6 &&
      field(s, 9, df, sizeof(df)) && strlen(df) >= 6) {
    sHour   = (tf[0]-'0')*10 + (tf[1]-'0');
    sMinute = (tf[2]-'0')*10 + (tf[3]-'0');
    sSecond = (tf[4]-'0')*10 + (tf[5]-'0');
    sDay    = (df[0]-'0')*10 + (df[1]-'0');
    sMonth  = (df[2]-'0')*10 + (df[3]-'0');
    sYear   = 2000 + (df[4]-'0')*10 + (df[5]-'0');
    sFix.timeValid = true;
    snprintf(sFix.utc, sizeof(sFix.utc), "%04u-%02u-%02u %02u:%02u:%02u",
             sYear, sMonth, sDay, sHour, sMinute, sSecond);
    syncClock();
  }
}

// Fix data: quality, satellites, dilution and altitude.
void parseGga(const char* s) {
  char f[16];
  if (field(s, 6, f, sizeof(f))) sFix.quality    = atoi(f);
  if (field(s, 7, f, sizeof(f))) sFix.satellites = atoi(f);
  if (field(s, 8, f, sizeof(f))) sFix.hdop       = atof(f);
  if (field(s, 9, f, sizeof(f))) sFix.altitude   = atof(f);
}

// The per-satellite story, up to four SVs per sentence. An aging map rather
// than message-cycle bookkeeping: entries update in place and the reader
// ignores anything not refreshed lately — constellations may interleave
// their blocks however they like.
#if HAS_LVGL_UI                          // only the sky view reads this
constexpr size_t kMaxSvs = 20;
Gps::Sv sSvs[kMaxSvs];
portMUX_TYPE sSvMux = portMUX_INITIALIZER_UNLOCKED;

void parseGsv(const char* s) {
  char f[16];
  for (int k = 0; k < 4; k++) {
    if (!field(s, 4 + 4 * k, f, sizeof(f)) || !f[0]) continue;
    const uint8_t id = (uint8_t)atoi(f);
    if (!id) continue;
    uint8_t cn0 = 0;
    if (field(s, 7 + 4 * k, f, sizeof(f)) && f[0]) cn0 = (uint8_t)atoi(f);
    const uint32_t nowMs = millis();
    taskENTER_CRITICAL(&sSvMux);
    size_t slot = 0;
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < kMaxSvs; i++) {
      if (sSvs[i].seenMs && sSvs[i].id == id &&
          sSvs[i].talker[0] == s[1] && sSvs[i].talker[1] == s[2]) { slot = i; break; }
      // Age, never the raw stamp: absolute comparison inverts after the
      // 49.7-day wrap and evicts the freshest SVs while ghosts hold slots.
      const uint32_t age = sSvs[i].seenMs ? nowMs - sSvs[i].seenMs : UINT32_MAX;
      if (age >= oldestAge) { oldestAge = age; slot = i; }
    }
    Gps::Sv& v = sSvs[slot];
    v.talker[0] = s[1]; v.talker[1] = s[2]; v.talker[2] = 0;
    v.id = id;
    v.cn0 = cn0;
    v.seenMs = millis() ? millis() : 1;
    taskEXIT_CRITICAL(&sSvMux);
  }
}
#endif // HAS_LVGL_UI

void parse(const char* s, uint16_t len) {
  if (!checksumOk(s, len)) return;
  sFix.sentences++;
  sLastSentenceMs = millis();
  // The talker id varies with the constellation (GP, GN, GL, GA), so match on
  // the sentence type instead of the whole prefix.
  if      (strncmp(s + 3, "RMC", 3) == 0) parseRmc(s);
  else if (strncmp(s + 3, "GGA", 3) == 0) parseGga(s);
#if HAS_LVGL_UI
  else if (strncmp(s + 3, "GSV", 3) == 0) parseGsv(s);
#endif
}

#if GPS_NAP == GPS_NAP_PMREQ
// The way back from backup mode, which is not a command. The receiver was
// asked to wake on an edge on its own receive line (UbxFrame.h quotes the
// wording), so anything at all sent down the port is the wake — one byte is
// enough, because a UART frame's start bit is an edge whatever the byte
// carries. 0xFF is chosen for being the start of nothing: it is neither a
// sentence's '$' nor a UBX preamble, so a receiver that was awake all along
// discards it without half-parsing anything.
//
// The stamp is taken here rather than at the call sites, so the interval the
// repeat below keeps is measured from every byte this file sends and not only
// from the repeats' own.
void wakeNudge() {
  sSerial.write((uint8_t)0xFF);
  sLastNudgeMs = millis();
}
#endif

// Opens the receiver's port, and says whether it came back.
//
// HardwareSerial::begin() returns void: on failure it logs inside the core,
// leaves its uart_t null and goes on answering available() with 0 for ever —
// a receiver that is permanently silent for a reason nothing outside the core
// could see. operator bool() is uartIsDriverInstalled(), which is the answer
// nothing here used to ask for. It matters most where the port is torn down
// and rebuilt on a schedule — the switched-rail board reopens it at the end of
// every rest, on a part with no PSRAM — because there a failed install is not
// something the next boot comes along to fix.
bool openPort() {
  sLastPortTryMs = millis();
  sSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  const bool up = (bool)sSerial;
  // On the way into the fault only, not once per retry. The reader tries again
  // every five seconds for as long as the receiver is enabled, and seventeen
  // thousand identical lines a day would bury whatever else the log had to
  // say; the recovery announces itself at the call site.
  if (!up && !sFix.portFault)
    log_e("GNSS UART1 did not open (rx %d tx %d @ %d baud) — retrying every %us",
          PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD, (unsigned)(PORT_RETRY_MS / 1000));
  sFix.portFault = !up;
  return up;
}

// Carry a verdict to the receiver. *Which* mechanism a board gets was decided
// once, in Config.h (GPS_NAP); this function is those answers and nothing
// else. It is idempotent, and it must be: it is re-applied when a settings
// save re-asserts the enable line under a resting receiver.
void applyRest(bool rest) {
#if GPS_NAP == GPS_NAP_STANDBY
  // heltec-v4 and thinknode-m9. The standby line has only ever been driven one
  // way here — high at switch-on, to force awake a receiver that would
  // otherwise arrive asleep and parse as absent — and low is the other half of
  // that same sentence. It is the whole mechanism on these two boards:
  // neither part is u-blox (a Quectel L76K on the V4's expansion kit, an
  // ATGM336H on the M9), so a UBX message would be a protocol they have never
  // heard of, and cutting PIN_GPS_EN would not be a nap but the operator's own
  // off switch — it takes the receiver's memory with it and makes every wake a
  // cold start.
  digitalWrite(PIN_GPS_STANDBY, rest ? LOW : HIGH);
#elif GPS_NAP == GPS_NAP_RAIL
  // tbeam. The power-management chip already has a real cut for this rail —
  // AXP192 LDO3 / AXP2101 ALDO3, the one runtime rail switch in this firmware
  // and the switch the operator's own GNSS setting already throws. Nothing
  // needs adding to reach it, and it is a harder off than any message. The
  // receiver's backup supply is not on this rail (Gps.h), so it keeps its
  // almanac across the cut and a wake is a warm start rather than a cold one —
  // which is the fact this cadence rests on, and the one to confirm on a
  // bench.
  //
  // The port goes down with the rail, deliberately: leaving the processor's
  // transmit line driving high into an unpowered receiver pushes current
  // through that part's protection diodes, which is how something that is
  // supposed to be off ends up half on.
  //
  // Which makes the reopen a driver install on every wake — 256 B of receive
  // ring, a twenty-entry event queue and the driver's own object, freed and
  // reallocated once a minute on the one board here with no PSRAM. That is
  // survivable (the same sizes are asked for each time, so the block just
  // freed is the block taken) but it is not guaranteed, and begin() has no way
  // to say so. openPort() asks; the reader retries what did not come back.
  if (rest) {
    sSerial.end();
    Pmu::gpsPower(false);
  } else {
    Pmu::gpsPower(true);
    openPort();
  }
#elif GPS_NAP == GPS_NAP_PMREQ
  // t-deck. No enable line, no standby line, no switched rail: the receiver's
  // own protocol is the only off-switch that exists on this board, and this is
  // the one binary message this firmware has ever sent a GNSS module.
  //
  // The schedule is this node's — the wake is a byte on the receiver's own
  // receive line, sent when the policy says the rest is over — and the
  // duration handed to the receiver is only a backstop under it, because on
  // this board there is no rail to power-cycle a module that failed to hear
  // the byte. UbxFrame.h argues that at length.
  if (rest) {
    uint8_t frame[Ubx::kPmreqFrame];
    const size_t n = Ubx::pmreqBackup(sRestMs, Ubx::kWakeUartRx, frame, sizeof(frame));
    if (n) {
      sSerial.write(frame, n);
      // From here on this run has something to recover from, so the repeat
      // below is allowed to prod a receiver that stays quiet. Before it, the
      // port stays as silent as it was before this milestone.
      sEverAsked = true;
    }
  } else {
    wakeNudge();
  }
#else
  // Nothing fitted to ask. The policy still runs and still decides — which is
  // what keeps this a per-board hardware fact rather than a per-board rule —
  // and the driver simply has nothing to send.
  (void)rest;
#endif
}

void resetState() {
  Gps::Fix cleared;
  cleared.enabled = sFix.enabled;
  cleared.clockSet = sFix.clockSet;             // the clock stays set
  sFix = cleared;
  sLen = 0; sOverflow = false;
  sYear = 0;
}

void task(void*) {
  sTaskHandle = xTaskGetCurrentTaskHandle();
  Watchdog::watch();
  // Guarded whole rather than per statement: the loop below uses continue,
  // which cannot cross a lambda. A receiver that cannot be parsed for want
  // of memory costs a fix, not the node (Diag.h).
  for (;;) {
    Diag::guard("the gps task", [&] {
      for (;;) {
        // The inner loop is the one that spins, as in AutoInterface.
        Watchdog::feed();
        // The lock is taken before the enabled test, not after it: setEnabled()
        // closes the UART while holding the same lock, and a reader that decided
        // to run just before that would otherwise go on to read a port that has
        // since been shut down.
        Sys::Lock held(sLock);
        const bool on = sFix.enabled;
        if (on) {
          // A port that did not come back reads as a receiver that has nothing
          // to say — for ever, since available() answers 0 on a driver that
          // was never installed and nothing else here would ever reopen it.
          // Before the read, so a pass that succeeds here still reads on it.
          // On its own clock rather than the pass's, which is taken after the
          // read below: every stamp `now` is compared against is written
          // *inside* that read, so a `now` from before it would run backwards
          // on any pass that parsed a sentence.
          if (sFix.portFault) {
            const uint32_t tried = millis();
            if (tried - sLastPortTryMs >= PORT_RETRY_MS && openPort())
              log_i("GNSS UART1 back");
          }
          // Bounded per pass so a chatty receiver cannot monopolise the task.
          // 9600 baud is under 1 KB/s, and this runs ten times a second.
          uint16_t budget = 256;
          while (sSerial.available() && budget--) {
            char c = sSerial.read();
            if (c == '$') { sLen = 0; sOverflow = false; }
            if (c == '\r' || c == '\n') {
              if (!sOverflow && sLen > 5) { sLine[sLen] = 0; parse(sLine, sLen); }
              sLen = 0; sOverflow = false;
              continue;
            }
            if (sLen < NMEA_MAX - 1) sLine[sLen++] = c;
            else                     sOverflow = true;
          }
          const uint32_t now = millis();
          // Drop a stale fix rather than reporting a position the receiver no
          // longer stands behind — unless the silence is ours. The rule, and
          // the argument for it, are GnssDutyPolicy's: this is the schedule's
          // own arithmetic, and it is pinned on the host with the schedule
          // rather than being three lines that only ever run beside a board.
          if (GnssDutyPolicy::dropStaleFix(sResting, sFix.valid, now, sLastFixMs, sTrackFromMs))
            sFix.valid = false;
          sFix.ageMs = sFix.sentences ? now - sLastSentenceMs : 0;

          // Whether the receiver may stop looking (GnssDutyPolicy.h). Asked on
          // the pass that already runs rather than from a task of its own:
          // this one owns the port, and a second task on the same UART would
          // have to be locked against this one anyway.
          GnssDutyPolicy::State st;
          st.role = (uint8_t)Power::role();
          // Not sFix.valid on its own: the question is whether the receiver
          // has found itself *again* since it was allowed to look.
          st.fix = GnssDutyPolicy::fixReacquired(sFix.valid, sLastFixMs, sTrackFromMs);
          // "Somebody is looking at where this node is". The screen state is
          // Power's — one answer to that question in the firmware — rather
          // than a second copy kept here; the claim's own expiry is the
          // schedule's, beside the rest it defers.
          st.navLit = GnssDutyPolicy::navLit(Power::screenDark(), now,
                                             sNavViewMs.load(std::memory_order_relaxed));

          const bool rest = sDuty.update(now, st) == GnssDutyPolicy::Verdict::Rest;
          if (rest != sResting) {
            sResting = rest;
            sFix.resting = rest;
#if GPS_NAP == GPS_NAP_PMREQ
            // What the receiver is told its own backstop timer is worth. Read
            // from the role that has just earned this rest, so the two never
            // disagree about how long it is.
            if (rest) sRestMs = GnssDutyPolicy::restMsFor(st.role);
#endif
            // Before the hardware is touched, so the window a re-asserted fix
            // is measured against starts at the wake and not after it.
            if (!rest) sTrackFromMs = now;
            applyRest(rest);
            log_i("GNSS receiver %s", rest ? "resting — it knows where it is"
                                           : "looking again");
          }
#if GPS_NAP == GPS_NAP_PMREQ
          // A receiver in backup mode says nothing, and the only thing that
          // ends that is an edge on its receive line. If a wake was missed the
          // node would otherwise count sentences for ever without seeing one,
          // and this board has no rail to power-cycle it with — so a receiver
          // that ought to be talking and is not gets prodded again, one byte
          // every five seconds.
          //
          // Only where this run has actually asked one to stop. The pins are
          // this board's Grove connector on the variant that fits no receiver,
          // and a node whose role does not duty-cycle never asks anything to
          // stop at all: on either of those the port must stay exactly as
          // silent as it was before this milestone, which is what the
          // settings page and the documentation both promise. A module left in
          // backup by a *previous* run is covered without a byte from here —
          // every request this firmware sends carries the rest's own duration
          // as a backstop, so such a module is awake again within one rest
          // interval whatever this run does (UbxFrame.h argues that at length).
          if (GnssDutyPolicy::nudgeDue(
                  sResting, sEverAsked, now,
                  GnssDutyPolicy::quietSince(sLastSentenceMs, sTrackFromMs),
                  sLastNudgeMs))
            wakeNudge();
#endif
        }
        held.release();
        if (on) {
          // A resting receiver sends nothing, so polling it ten times a second
          // is ten wake-ups for an empty buffer. Half a second still ends a
          // rest inside one screen paint, which is what the responsiveness
          // budget here actually is.
          vTaskDelay(pdMS_TO_TICKS(sResting ? 500 : 100));
        } else {
          // Disabled: nothing to poll for, so wait to be told rather than
          // waking 10x/s to reread a flag. setEnabled(true) gives the
          // notification directly; the timeout is only a fallback in case a
          // future caller flips the flag without going through it.
          ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        }
      }
    });
    vTaskDelay(pdMS_TO_TICKS(100));      // contained: wait, then back in
  }
}

} // namespace

namespace Gps {

bool enabled() { return sFix.enabled; }

void setEnabled(bool on) {
  Sys::Lock held(sLock);
  // The switches are driven before the already-in-that-state early return,
  // deliberately: at boot nothing has driven them yet, and "already off"
  // used to return here with the enable line floating — on a board whose
  // line idles toward on, a receiver drawing tens of mA under a UI that
  // said GNSS was disabled. Re-writing a pin to the level it already holds
  // costs nothing.
  //
  // A PMU board powers the receiver's rail through the chip; a board with a
  // plain enable line drives the line; a board with neither leaves the
  // receiver always on. Each call is a cheap no-op where it does not apply.
  Pmu::gpsPower(on);
#if PIN_GPS_EN >= 0
  pinMode(PIN_GPS_EN, OUTPUT);
  digitalWrite(PIN_GPS_EN, on ? GPS_EN_ACTIVE : !GPS_EN_ACTIVE);
#endif
  if (on == sFix.enabled) {
#if GPS_NAP == GPS_NAP_RAIL
    // The unconditional Pmu::gpsPower(true) above has just put the receiver's
    // rail back — on this board that rail *is* the nap, so a settings save
    // would otherwise wake a receiver the duty policy still believes is
    // resting and leave the two disagreeing until the rest happened to
    // expire. Put the verdict back instead.
    //
    // Only here. A standby line is not touched above, and re-sending a backup
    // request to a receiver already in backup would wake it with the first
    // byte's edge only to ask it to sleep again.
    if (on && sResting) applyRest(true);
#endif
    return;
  }
  if (on) {
#if PIN_GPS_STANDBY >= 0
    // Force the receiver awake: low means it may sleep, and a receiver that
    // arrives asleep parses as absent.
    pinMode(PIN_GPS_STANDBY, OUTPUT);
    digitalWrite(PIN_GPS_STANDBY, HIGH);
#endif
#if PIN_GPS_RST >= 0
    // Reset released, never asserted here: the receiver holds its almanac
    // through a power cycle and a reset would cost the warm start that
    // holding it is worth. Released is the opposite of whichever level the
    // board says asserts it — on most a /RST line released by driving it
    // high, on one the other way round, where driving it high held the
    // receiver in reset and the node counted sentences for ever without
    // seeing one.
    pinMode(PIN_GPS_RST, OUTPUT);
    digitalWrite(PIN_GPS_RST, !GPS_RST_ACTIVE);
#endif
    openPort();
    sFix.enabled = true;
    // The duty cycle starts from nothing on every switch-on: a receiver that
    // has just been given its rail back has found nothing yet, whatever the
    // policy was in the middle of before it was switched off.
    sDuty = GnssDutyPolicy();
    sResting = false;
    sTrackFromMs = millis();
#if GPS_NAP == GPS_NAP_PMREQ
    // Nothing is sent here. This used to write one byte unconditionally, to
    // wake a module a previous run might have left in backup — but a receiver
    // in backup is one this firmware put there, and every request it sends
    // carries the rest's own duration as a backstop, so such a module wakes by
    // itself within one rest interval. Sending the byte anyway cost a node
    // with no receiver fitted — where these pins are the case's expansion
    // connector — a transmission onto a port it had never driven before.
    sEverAsked = false;
    sLastNudgeMs = 0;
#endif
    if (sTaskHandle) xTaskNotifyGive(sTaskHandle);
    log_i("GNSS receiver on (UART1 rx %d tx %d @ %d baud)", PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD);
  } else {
    sSerial.end();
    sFix.enabled = false;
    sDuty = GnssDutyPolicy();
    sResting = false;
#if GPS_NAP == GPS_NAP_PMREQ
    sEverAsked = false;
#endif
    resetState();
    log_i("GNSS receiver off");
  }
}

void begin() {
  sLock = xSemaphoreCreateMutex();
  // Pinning TZ makes mktime() a UTC conversion, which is what NMEA gives.
  setenv("TZ", "UTC0", 1);
  tzset();
  setEnabled(settings.radio().gpsEnabled);
  // 4 KB, not 3: the first boot on hardware measured 1216 B of headroom at
  // 3072 with a receiver talking — enough to run, not enough to trust under
  // the guard's worst case (Diag.h). The margin is cheap; the overflow is not.
  Diag::startTask(task, "gps", 4096, nullptr, 1, 0);
}

Fix fix() {
  if (!sLock) return Fix{};
  Sys::Lock held(sLock);
  return sFix;
}

void navViewPainted() {
  // No lock and no read-modify-write: one 32-bit store, written on the display
  // task and read on the receiver's. A claim that lands one pass late costs a
  // tenth of a second of tracking, which is not worth a mutex on the path a
  // screen repaints from. Relaxed and not merely plain, so that stays true of
  // a build with link-time optimisation rather than by luck.
  const uint32_t now = millis();
  sNavViewMs.store(now ? now : 1, std::memory_order_relaxed);   // 0 is "never"
}

size_t skyView(Sv* out, size_t max) {
#if !HAS_LVGL_UI
  // No sky view on this build: a GPS board without a glass must not pay
  // interrupt-masked table scans forty times a second for a chart it
  // cannot draw.
  (void)out; (void)max;
  return 0;
#else
  const uint32_t now = millis();
  size_t w = 0;
  taskENTER_CRITICAL(&sSvMux);
  for (size_t i = 0; i < kMaxSvs && w < max; i++)
    if (sSvs[i].seenMs && now - sSvs[i].seenMs < 10000) out[w++] = sSvs[i];
  taskEXIT_CRITICAL(&sSvMux);
  return w;
#endif
}

} // namespace Gps

#else   // HAS_GPS == 0

namespace Gps {
void setEnabled(bool) {}
bool enabled() { return false; }
void begin() {}
void navViewPainted() {}
Fix fix() { return Fix{}; }
}

#endif
