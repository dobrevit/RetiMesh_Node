// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Gps.cpp — see Gps.h
// ============================================================================
#include "Gps.h"

#if HAS_GPS

#include "Pmu.h"
#include "Settings.h"
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <time.h>
#include "Diag.h"
#include "Lock.h"

namespace {

// The longest sentence we care about is well under this. Anything longer is
// discarded rather than truncated, so a partial line can never be parsed.
const uint16_t NMEA_MAX      = 100;
// A fix is reported stale rather than wrong once the receiver stops
// reasserting it.
const uint32_t FIX_TIMEOUT   = 10000;
// The receiver's clock is far better than ours, but re-adopting it every
// sentence would be pointless churn.
const uint32_t SYNC_INTERVAL = 3600000;

HardwareSerial   sSerial(1);
SemaphoreHandle_t sLock = nullptr;
Gps::Fix         sFix;
char             sLine[NMEA_MAX];
uint16_t         sLen = 0;
bool             sOverflow = false;
uint32_t         sLastFixMs = 0, sLastSentenceMs = 0, sLastSyncMs = 0;
uint16_t         sYear = 0;
uint8_t          sMonth = 0, sDay = 0, sHour = 0, sMinute = 0, sSecond = 0;

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

void parse(const char* s, uint16_t len) {
  if (!checksumOk(s, len)) return;
  sFix.sentences++;
  sLastSentenceMs = millis();
  // The talker id varies with the constellation (GP, GN, GL, GA), so match on
  // the sentence type instead of the whole prefix.
  if      (strncmp(s + 3, "RMC", 3) == 0) parseRmc(s);
  else if (strncmp(s + 3, "GGA", 3) == 0) parseGga(s);
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
  // Guarded whole rather than per statement: the loop below uses continue,
  // which cannot cross a lambda. A receiver that cannot be parsed for want
  // of memory costs a fix, not the node (Diag.h).
  for (;;) {
    Diag::guard("the gps task", [&] {
      for (;;) {
        // The lock is taken before the enabled test, not after it: setEnabled()
        // closes the UART while holding the same lock, and a reader that decided
        // to run just before that would otherwise go on to read a port that has
        // since been shut down.
        Sys::Lock held(sLock);
        if (sFix.enabled) {
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
          // Drop a stale fix rather than reporting a position the receiver no
          // longer stands behind.
          if (sFix.valid && millis() - sLastFixMs >= FIX_TIMEOUT) sFix.valid = false;
          sFix.ageMs = sFix.sentences ? millis() - sLastSentenceMs : 0;
        }
        held.release();
        vTaskDelay(pdMS_TO_TICKS(100));
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
  if (on == sFix.enabled) return;
  Pmu::gpsPower(on);
  if (on) {
    sSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    sFix.enabled = true;
    log_i("GNSS receiver on (UART1 rx %d tx %d @ %d baud)", PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD);
  } else {
    sSerial.end();
    sFix.enabled = false;
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
  Diag::startTask(task, "gps", 3072, nullptr, 1, 0);
}

Fix fix() {
  if (!sLock) return Fix{};
  Sys::Lock held(sLock);
  return sFix;
}

} // namespace Gps

#else   // HAS_GPS == 0

namespace Gps {
void setEnabled(bool) {}
bool enabled() { return false; }
void begin() {}
Fix fix() { return Fix{}; }
}

#endif
