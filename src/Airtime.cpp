// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Airtime.cpp — see Airtime.h
// ============================================================================
#include "Airtime.h"
#include <math.h>

void Airtime::configure(const Params& p) {
  _p = p;
  if (_p.sf < 5)  _p.sf = 5;
  if (_p.sf > 12) _p.sf = 12;
  if (_p.cr < 5)  _p.cr = 5;
  if (_p.cr > 8)  _p.cr = 8;
  if (_p.bwKhz <= 0.0f) _p.bwKhz = 125.0f;
}

float Airtime::symbolTimeMs() const {
  return (float)(1UL << _p.sf) / _p.bwKhz;      // 2^SF / BW(kHz) = ms
}

float Airtime::timeOnAirMs(size_t payloadBytes) const {
  const float tSym = symbolTimeMs();
  // Low data rate optimisation is mandatory when a symbol lasts over 16 ms.
  const int de = (tSym > 16.0f) ? 1 : 0;
  const int ih = _p.implicitHeader ? 1 : 0;
  const int crc = _p.crcOn ? 1 : 0;
  const int cr = _p.cr - 4;                     // 4/5..4/8 -> 1..4

  float num = 8.0f * (float)payloadBytes - 4.0f * _p.sf + 28.0f + 16.0f * crc - 20.0f * ih;
  float den = 4.0f * (float)(_p.sf - 2 * de);
  float payloadSymbols = 8.0f + fmaxf(ceilf(num / den) * (float)(cr + 4), 0.0f);

  float preamble = ((float)_p.preambleSyms + 4.25f) * tSym;
  return preamble + payloadSymbols * tSym;
}

void Airtime::rollover(uint32_t nowMs) {
  const uint32_t stamp = nowMs / BIN_MS;
  if (!_started) { _started = true; _lastBinStamp = stamp; return; }
  if (stamp == _lastBinStamp) return;

  // Zero every bin between the last one we touched and now. More than a
  // window's worth of silence means the whole record is stale.
  uint32_t elapsed = stamp - _lastBinStamp;     // unsigned: survives millis() wrap
  if (elapsed >= BINS) {
    for (uint16_t i = 0; i < BINS; i++) _bins[i] = 0.0f;
  } else {
    for (uint32_t i = 1; i <= elapsed; i++) _bins[(_lastBinStamp + i) % BINS] = 0.0f;
  }
  _lastBinStamp = stamp;
}

void Airtime::addTx(uint32_t nowMs, float airMs) {
  if (airMs <= 0.0f) return;
  rollover(nowMs);
  _bins[binOf(nowMs)] += airMs;
}

float Airtime::shortTermUtil(uint32_t nowMs) {
  rollover(nowMs);
  const uint16_t cur = binOf(nowMs);
  const uint16_t prev = (uint16_t)((cur + BINS - 1) % BINS);
  return (_bins[cur] + _bins[prev]) / (2.0f * (float)BIN_MS);
}

float Airtime::longTermUtil(uint32_t nowMs) {
  rollover(nowMs);
  float sum = 0.0f;
  for (uint16_t i = 0; i < BINS; i++) sum += _bins[i];
  return sum / (float)((uint32_t)BINS * BIN_MS);
}

float Airtime::budgetUsed(uint32_t nowMs, uint8_t limitPct) {
  if (limitPct == 0) return 0.0f;
  return longTermUtil(nowMs) / ((float)limitPct / 100.0f);
}

bool Airtime::locked(uint32_t nowMs, uint8_t limitPct) {
  if (limitPct == 0) return false;
  return longTermUtil(nowMs) >= (float)limitPct / 100.0f;
}

uint32_t Airtime::retryAfterS(uint32_t nowMs, uint8_t limitPct) {
  if (!locked(nowMs, limitPct)) return 0;
  // Bins expire oldest first; find how many must roll off before the total
  // drops under the allowance.
  const float allowance = ((float)limitPct / 100.0f) * (float)((uint32_t)BINS * BIN_MS);
  float total = 0.0f;
  for (uint16_t i = 0; i < BINS; i++) total += _bins[i];
  const uint16_t cur = binOf(nowMs);
  for (uint16_t age = 0; age < BINS; age++) {
    // The oldest bin is the one just after the current one.
    const uint16_t idx = (uint16_t)((cur + 1 + age) % BINS);
    total -= _bins[idx];
    if (total < allowance) {
      const uint32_t untilNextBin = BIN_MS - (nowMs % BIN_MS);
      return (untilNextBin + (uint32_t)age * BIN_MS) / 1000U;
    }
  }
  return (uint32_t)BINS * BIN_MS / 1000U;
}

uint32_t Airtime::slotMs() const {
  float slot = (float)SLOT_SYMBOLS * symbolTimeMs();
  if (slot < (float)SLOT_MIN_MS) slot = (float)SLOT_MIN_MS;
  if (slot > (float)SLOT_MAX_MS) slot = (float)SLOT_MAX_MS;
  return (uint32_t)(slot + 0.5f);
}

uint8_t Airtime::cwBand(float shortTerm) const {
  const int pct = (int)(shortTerm * 100.0f);
  if (pct <= (int)BAND_1_MAX_PCT) return 1;
  // Linear across the remaining bands, as RNode does.
  const int lo = BAND_1_MAX_PCT, hi = BAND_N_MIN_PCT;
  int band = 2 + ((pct - lo) * (CW_BANDS - 2)) / (hi - lo > 0 ? hi - lo : 1);
  if (band < 2) band = 2;
  if (band > (int)CW_BANDS) band = CW_BANDS;
  return (uint8_t)band;
}

void Airtime::contentionWindow(float shortTerm, uint8_t& cwMin, uint8_t& cwMax) const {
  const uint8_t band = cwBand(shortTerm);
  cwMin = (uint8_t)((band - 1) * CW_PER_BAND);
  cwMax = (uint8_t)(band * CW_PER_BAND - 1);
}

void Airtime::reset() {
  for (uint16_t i = 0; i < BINS; i++) _bins[i] = 0.0f;
  _started = false;
  _lastBinStamp = 0;
}
