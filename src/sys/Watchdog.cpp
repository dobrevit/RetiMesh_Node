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

#include "Watchdog.h"

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "Config.h"

namespace Watchdog {
namespace {
bool sArmed = false;
}

void begin() {
  // Reconfigure rather than init: the IDF has already started the watchdog
  // (CONFIG_ESP_TASK_WDT_INIT) with a 5-second timeout and the idle tasks
  // subscribed. All that changes here is the timeout, which several of our
  // passes exceed honestly, and which would otherwise reboot a working node
  // the first time a panel refreshed slowly.
  esp_task_wdt_config_t cfg = {
    .timeout_ms = WATCHDOG_TIMEOUT_S * 1000U,
    .idle_core_mask = 0,          // the idle tasks are IDF's business, not ours
    .trigger_panic = true,        // a miss must reset: a hang costs a field trip
  };
  const esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  sArmed = (err == ESP_OK);
  if (sArmed)
    log_i("watchdog: tasks that stop reporting for %u s restart the node", WATCHDOG_TIMEOUT_S);
  else
    log_e("watchdog: could not be configured (%s) — a hung task will not be caught "
          "and the node would have to be power-cycled by hand", esp_err_to_name(err));
}

void watch() {
  if (!sArmed) return;
  const esp_err_t err = esp_task_wdt_add(nullptr);
  // Already subscribed is not a failure: a task that restarts its loop after a
  // contained throw calls this again, and that is the shape we want.
  if (err != ESP_OK && err != ESP_ERR_INVALID_ARG)
    log_w("watchdog: \"%s\" could not subscribe (%s); it runs unsupervised",
          pcTaskGetName(nullptr), esp_err_to_name(err));
}

void feed() {
  if (sArmed) esp_task_wdt_reset();
}

void pause() {
  if (sArmed) esp_task_wdt_delete(nullptr);
}

void resume() {
  if (sArmed) esp_task_wdt_add(nullptr);
}

bool armed() { return sArmed; }

}  // namespace Watchdog
