// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Pmu.cpp — see Pmu.h
// ============================================================================
#include "Pmu.h"

#if HAS_PMU

#include <Wire.h>
// XPowersLib exposes one chip class when XPOWERS_CHIP_* is defined and *all*
// of them when none is — and this board comes in both flavours, so define
// nothing and pick the right class at runtime.
#include <XPowersLib.h>

namespace {
XPowersLibInterface* sPmu = nullptr;
const char* sModel = "none";
bool sGpsOn = false;
}

namespace Pmu {

bool present() { return sPmu != nullptr; }
const char* model() { return sModel; }
bool gpsPowered() { return sGpsOn; }

bool begin() {
  Wire.begin(PIN_PMU_SDA, PIN_PMU_SCL);

  // Both parts live at the same address; try each and keep the one that
  // recognises its own chip id.
  XPowersAXP2101* axp2101 = new XPowersAXP2101();
  if (axp2101->init(Wire, PIN_PMU_SDA, PIN_PMU_SCL, AXP2101_SLAVE_ADDRESS)) {
    sPmu = axp2101; sModel = "AXP2101";
  } else {
    delete axp2101;
    XPowersAXP192* axp192 = new XPowersAXP192();
    if (axp192->init(Wire, PIN_PMU_SDA, PIN_PMU_SCL, AXP192_SLAVE_ADDRESS)) {
      sPmu = axp192; sModel = "AXP192";
    } else {
      delete axp192;
      log_e("no power-management chip answered on I2C %d/%d — the radio and "
            "GPS rails stay off and nothing will be found on SPI",
            PIN_PMU_SDA, PIN_PMU_SCL);
      return false;
    }
  }

  // Rails, by the name each chip gives them:
  //   AXP192   LDO2 = LoRa, LDO3 = GPS, DCDC1 = OLED
  //   AXP2101  ALDO2 = LoRa, ALDO3 = GPS, DCDC1 = OLED/ESP32
  if (strcmp(sModel, "AXP192") == 0) {
    sPmu->setPowerChannelVoltage(XPOWERS_LDO2, 3300);
    sPmu->enablePowerOutput(XPOWERS_LDO2);              // transceiver
    sPmu->setPowerChannelVoltage(XPOWERS_DCDC1, 3300);
    sPmu->enablePowerOutput(XPOWERS_DCDC1);             // display
    sPmu->setPowerChannelVoltage(XPOWERS_LDO3, 3300);
    sPmu->disablePowerOutput(XPOWERS_LDO3);             // GPS: off for now
  } else {
    sPmu->setPowerChannelVoltage(XPOWERS_ALDO2, 3300);
    sPmu->enablePowerOutput(XPOWERS_ALDO2);             // transceiver
    sPmu->setPowerChannelVoltage(XPOWERS_ALDO3, 3300);
    sPmu->disablePowerOutput(XPOWERS_ALDO3);            // GPS: off for now
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
  const uint8_t rail = (strcmp(sModel, "AXP192") == 0) ? XPOWERS_LDO3 : XPOWERS_ALDO3;
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
