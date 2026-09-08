// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Pmu.cpp — see Pmu.h
// ============================================================================
#include "Pmu.h"

#if HAS_PMU

#include <Wire.h>
#include "I2cReg.h"
// XPowersLib exposes one chip class when XPOWERS_CHIP_* is defined and *all*
// of them when none is — and this board comes in both flavours, so define
// nothing and pick the right class at runtime.
#include <XPowersLib.h>

// The receiver's rail is the one that gets switched after boot — GnssDutyPolicy
// naps the receiver by cutting it (Config.h, GPS_NAP_RAIL) — so it is the one
// rail that must not be shared. A board that gave it the same regulator as its
// sensors or its card would cut those every time the receiver rested, and the
// symptom would be a clock that loses time or a card that vanishes, an hour
// into a duty cycle, on a board where every part is individually fine.
//
// The other roles may legitimately share: they are all switched on once here
// and never again, so a board whose panel and sensors hang off one regulator
// says so twice and nothing is harmed. That is why this asserts the one case
// that bites rather than pairwise distinctness across the six.
#if HAS_GPS
static_assert(PMU_RAIL_GPS == PMU_RAIL_NONE ||
              (PMU_RAIL_GPS != PMU_RAIL_RADIO   &&
               PMU_RAIL_GPS != PMU_RAIL_DISPLAY &&
               PMU_RAIL_GPS != PMU_RAIL_SENSORS &&
               PMU_RAIL_GPS != PMU_RAIL_CARD    &&
               PMU_RAIL_GPS != PMU_RAIL_MODULE),
              "PMU_RAIL_GPS names the same regulator as another role: napping the "
              "receiver cuts that role's rail with it. Check the board header.");
#endif

namespace {
XPowersLibInterface* sPmu = nullptr;
const char* sModel = "none";
bool sGpsOn = false;

// One of the chip's regulators, set to 3V3 and switched. Every rail on every
// board here runs at 3V3, so the voltage is written before the switch either
// way — a rail that is off still has to come up at the right voltage when
// something asks for it later, which is how the receiver's rail is treated.
//
// A board that does not name a rail (PMU_RAIL_NONE) is not saying "off": it is
// saying the chip's own power-up state is the right one and nothing here
// should have an opinion. Config.h holds the names.
//
// Every one of these calls returns a bool and every one of them can be refused
// — a protected channel, a voltage off the chip's step grid. They were being
// discarded, which on a board whose parts all hang off these rails is the one
// thing in begin() with no observability: the charge terms below are read back
// and reported, and the rails that decide whether the radio exists at all were
// not. A refused rail and an absent part read exactly the same on a bench.
bool railTo(const char* what, uint8_t ch, bool on) {
  if (ch == PMU_RAIL_NONE) return true;
  const bool volt = sPmu->setPowerChannelVoltage(ch, 3300);
  const bool sw   = on ? sPmu->enablePowerOutput(ch) : sPmu->disablePowerOutput(ch);
  if (!volt || !sw)
    log_w("%s: the %s rail (channel %u) did not take — voltage %s, switch %s. An "
          "unpowered rail reads exactly like a part that is not fitted",
          sModel, what, (unsigned)ch, volt ? "ok" : "refused", sw ? "ok" : "refused");
  return volt && sw;
}
}

namespace Pmu {

bool present() { return sPmu != nullptr; }
const char* model() { return sModel; }
bool gpsPowered() { return sGpsOn; }

bool begin() {
  // The PMU's own pins decide which bus it is on, and one place decides what a
  // pair of pins means (I2cReg.h). On the T-Beam they are the display's pins,
  // so this is the board's general bus; on the T-Beam Supreme the PMU and the
  // clock have a pair to themselves and the panel is elsewhere, where starting
  // Wire on the PMU's pins — as this did — would have moved the panel onto the
  // PMU's wires and left it reading as a panel that is not fitted.
  //
  // One thing this changes on the T-Beam, where the two are the same pair:
  // the bus now starts at I2C_HZ rather than at the core's 100 kHz default,
  // which is what `Wire.begin(sda, scl)` with no frequency asked for
  // (esp32-hal-i2c.c substitutes 100000 for a zero). Both PMUs and the panel
  // are rated for 400 kHz, and the panel's own driver already raises the
  // clock to that around every frame and drops it afterwards — so the bus was
  // changing speed under the PMU regardless. It is named here because it is a
  // change on a board that is deployed, and a NAK on that shared bus is
  // already the known cause of charge terms not applying.
  bool busUp = false;
  TwoWire& bus = I2cReg::busFor(PIN_PMU_SDA, PIN_PMU_SCL, I2C_HZ, &busUp);
  if (!busUp) {
    log_e("the I2C host for the power-management chip (SDA %d, SCL %d) would "
          "not start — every rail this board switches stays as the chip left "
          "it", PIN_PMU_SDA, PIN_PMU_SCL);
    return false;
  }

  // Both parts live at the same address; try each and keep the one that
  // recognises its own chip id. XPowersLib calls begin() on the bus itself,
  // which on an already-started host is a warning and a no-op in the core
  // (Wire.cpp, "Bus already started in Master Mode") — the pins it was given
  // are the ones above, so there is nothing for it to move.
  XPowersAXP2101* axp2101 = new XPowersAXP2101();
  if (axp2101->init(bus, PIN_PMU_SDA, PIN_PMU_SCL, AXP2101_SLAVE_ADDRESS)) {
    sPmu = axp2101; sModel = "AXP2101";
  } else {
    delete axp2101;
    XPowersAXP192* axp192 = new XPowersAXP192();
    if (axp192->init(bus, PIN_PMU_SDA, PIN_PMU_SCL, AXP192_SLAVE_ADDRESS)) {
      sPmu = axp192; sModel = "AXP192";
    } else {
      delete axp192;
      log_e("no power-management chip answered on I2C %d/%d — the radio and "
            "GPS rails stay off and nothing will be found on SPI",
            PIN_PMU_SDA, PIN_PMU_SCL);
      return false;
    }
  }

  // Rails. The AXP192 is one board's part and keeps its names here; the
  // AXP2101 is carried by more than one board and wired differently on each,
  // so its channels come from the board header through Config.h.
  //   AXP192   LDO2 = LoRa, LDO3 = GPS, DCDC1 = OLED
  if (strcmp(sModel, "AXP192") == 0) {
    railTo("radio",   XPOWERS_LDO2,  true);
    railTo("display", XPOWERS_DCDC1, true);
    railTo("GNSS",    XPOWERS_LDO3,  false);            // off for now
  } else {
    railTo("radio",   PMU_RAIL_RADIO,   true);
    // Only where there is a receiver. PMU_RAIL_GPS keeps a real default for
    // the boards that have one, so on a PMU board with no receiver it would
    // name a regulator that board uses for something else — and this is the
    // one write in this function that turns a rail off.
#if HAS_GPS
    railTo("GNSS",    PMU_RAIL_GPS,     false);         // off for now
#endif
    railTo("display", PMU_RAIL_DISPLAY, true);
    railTo("sensors", PMU_RAIL_SENSORS, true);          // and the clock
    railTo("card",    PMU_RAIL_CARD,    true);
    railTo("module",  PMU_RAIL_MODULE,  true);          // plug-in radio socket
  }
  sGpsOn = false;

  sPmu->enableBattDetection();
  sPmu->enableBattVoltageMeasure();

  // Charging. The chip does it, but the terms are ours to set: a 4.2 V target
  // (the standard for the 18650 these boards carry) and a conservative current
  // — 500 mA on an AXP2101, 450 mA on an AXP192, which is the nearest step it
  // offers. Both are under 0.25 C for a typical cell and inside what a USB
  // port will give. Left at the chip's power-on defaults the current can be
  // low enough that a flat cell barely gains on a running node.
  //
  // The input limit matters as much as the charge current: VBUS feeds the whole
  // board, and a running node takes 120-250 mA of it — more while transmitting
  // — before the charger sees anything, so this ceiling is what actually
  // decides whether a flat cell gains. It is set explicitly rather than
  // inherited, but it is NOT raised on spec: 500 mA is all an unknown USB
  // source is obliged to give, and a laptop port asked for more can current-
  // limit, disconnect or brown out. PMU_VBUS_LIMIT_MA (Config.h) raises it
  // where the supply is known.
  //
  // The input voltage limit goes in alongside it. If the source cannot hold
  // VBUS above that floor the chip reduces its own draw instead of pulling the
  // rail down, which is what makes any ceiling survivable on a supply that
  // turns out to be weaker than expected.
  const bool axp192      = strcmp(sModel, "AXP192") == 0;
  const uint8_t wantVolt = axp192 ? (uint8_t)XPOWERS_AXP192_CHG_VOL_4V2
                                  : (uint8_t)XPOWERS_AXP2101_CHG_VOL_4V2;
  const uint8_t wantCurr = axp192 ? (uint8_t)XPOWERS_AXP192_CHG_CUR_450MA
                                  : (uint8_t)XPOWERS_AXP2101_CHG_CUR_500MA;
  // Largest step the chip offers that does not exceed what we were asked for
  unsigned limMa = 500;
  uint8_t  wantLim = (uint8_t)XPOWERS_AXP2101_VBUS_CUR_LIM_500MA;
  if (axp192) {
    wantLim = (uint8_t)XPOWERS_AXP192_VBUS_CUR_LIM_500MA;    // its maximum
  } else if (PMU_VBUS_LIMIT_MA >= 2000) { wantLim = (uint8_t)XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA; limMa = 2000; }
  else if   (PMU_VBUS_LIMIT_MA >= 1500) { wantLim = (uint8_t)XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA; limMa = 1500; }
  else if   (PMU_VBUS_LIMIT_MA >= 1000) { wantLim = (uint8_t)XPOWERS_AXP2101_VBUS_CUR_LIM_1000MA; limMa = 1000; }
  else if   (PMU_VBUS_LIMIT_MA >=  900) { wantLim = (uint8_t)XPOWERS_AXP2101_VBUS_CUR_LIM_900MA;  limMa =  900; }
  const uint8_t wantVin  = axp192 ? (uint8_t)XPOWERS_AXP192_VBUS_VOL_LIM_4V4
                                  : (uint8_t)XPOWERS_AXP2101_VBUS_VOL_LIM_4V36;
  const unsigned currMa  = axp192 ? 450 : 500;

  // Each of these is an I2C write that can be refused — the PMU shares the bus
  // with the OLED. Every setter runs, and then every register is read back
  // unconditionally: short-circuiting on the first failure would leave us
  // unable to say which of the three actually took, which is the whole point
  // of checking. Announcing terms the chip never accepted would hide exactly
  // the fault worth knowing about.
  const bool setVolt = sPmu->setChargeTargetVoltage(wantVolt);
  const bool setCurr = sPmu->setChargerConstantCurr(wantCurr);
  const bool setLim  = sPmu->setVbusCurrentLimit(wantLim);
  sPmu->setVbusVoltageLimit(wantVin);                   // returns void

  const bool okVolt = setVolt && sPmu->getChargeTargetVoltage() == wantVolt;
  const bool okCurr = setCurr && sPmu->getChargerConstantCurr() == wantCurr;
  const bool okLim  = setLim  && sPmu->getVbusCurrentLimit()    == wantLim;
  const bool chargeOk = okVolt && okCurr && okLim;

  // Hand the indicator LED back to the charger, which blinks it while current
  // is going into the cell and settles when it is full. It is the only signal
  // a node gives without a screen or a network, and switching it off — as this
  // did — makes a charging board look dead.
  sPmu->setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);

  char charge[56];
  if (chargeOk) snprintf(charge, sizeof(charge), "charging at up to %u mA (input limit %u mA)", currMa, limMa);
  else          snprintf(charge, sizeof(charge), "CHARGE TERMS NOT APPLIED");
  log_i("%s power-management chip: battery %.2f V%s, %s, radio rail on, GPS rail off",
        sModel, sPmu->getBattVoltage() / 1000.0f,
        sPmu->isCharging() ? " (charging)" : (sPmu->isBatteryConnect() ? "" : ", no cell"),
        charge);
  if (!chargeOk)
    log_w("%s: these charge terms did not read back as set:%s%s%s — the chip keeps whatever it "
          "held before, and a flat cell may not gain. A NAK on the I2C bus it shares with the "
          "display is the usual cause.", sModel,
          okVolt ? "" : " target voltage", okCurr ? "" : " charge current",
          okLim  ? "" : " input limit");
  delay(50);                                            // let the rails settle
  return true;
}

Battery battery() {
  Battery b;
  if (!sPmu) return b;
  b.present  = sPmu->isBatteryConnect();
  b.charging = sPmu->isCharging();
  b.volts    = sPmu->getBattVoltage() / 1000.0f;
  int pct    = sPmu->getBatteryPercent();
  b.percent  = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
  return b;
}

void gpsPower(bool on) {
  if (!sPmu) return;
  const uint8_t rail = (strcmp(sModel, "AXP192") == 0) ? (uint8_t)XPOWERS_LDO3
                                                       : (uint8_t)PMU_RAIL_GPS;
  // A board that names no receiver rail has no rail to cut, and the duty
  // policy's nap is then not this — GPS_NAP in Config.h picks what it is.
  if (rail == PMU_RAIL_NONE) return;
  if (on) sPmu->enablePowerOutput(rail);
  else    sPmu->disablePowerOutput(rail);
  sGpsOn = on;
}

} // namespace Pmu

#else   // HAS_PMU == 0

namespace Pmu {
bool begin() { return false; }
bool present() { return false; }
const char* model() { return "none"; }
Battery battery() { return Battery{}; }
void gpsPower(bool) {}
bool gpsPowered() { return false; }
}

#endif
