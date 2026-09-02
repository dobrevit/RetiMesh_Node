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

// ============================================================================
//  UiGps.cpp — the receiver's whole story on one screen
//
//  Everything Gps::fix() knows, refreshed each second while the screen shows:
//  the fix and its quality, the position, the clock and whether the node has
//  adopted it. The map this screen will one day carry is its own
//  investigation; the readings come first.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI && HAS_GPS

#include <Arduino.h>
#include "Gps.h"
#include "Settings.h"

namespace {

lv_obj_t* sTable = nullptr;

void refresh(lv_timer_t*) {
  if (!sTable || !lv_obj_is_valid(sTable)) return;
  const Gps::Fix f = Gps::fix();
  char v[48];
  size_t row = 0;
  auto put = [&](const char* k, const char* val) {
    lv_table_set_cell_value(sTable, row, 0, k);
    lv_table_set_cell_value(sTable, row, 1, val);
    row++;
  };
  put("Receiver", f.enabled ? "on" : "off (settings: radio)");
  put("Fix", f.valid ? "valid" : (f.sentences ? "searching" : "no data"));
  snprintf(v, sizeof(v), "%u", f.satellites);          put("Satellites", v);
  if (f.valid) {
    snprintf(v, sizeof(v), "%.6f", f.latitude);        put("Latitude", v);
    snprintf(v, sizeof(v), "%.6f", f.longitude);       put("Longitude", v);
    snprintf(v, sizeof(v), "%.1f m", (double)f.altitude);  put("Altitude", v);
    snprintf(v, sizeof(v), "%.1f", (double)f.hdop);        put("HDOP", v);
    snprintf(v, sizeof(v), "%.1f km/h", (double)f.speedKmh); put("Speed", v);
  }
  put("UTC", f.timeValid ? f.utc : "—");
  put("Node clock", f.clockSet ? "set from GNSS" : "not set");
  snprintf(v, sizeof(v), "%lu", (unsigned long)f.sentences); put("Sentences", v);
  put("Shares position", settings.radio().gpsSharePosition ? "yes" : "no");
}

} // namespace

namespace Ui {

void openGps() {
  lv_obj_t* body = newScreen("GNSS");
  sTable = lv_table_create(body);
  lv_obj_set_width(sTable, lv_pct(100));
  lv_table_set_column_width(sTable, 0, 92);
  lv_table_set_column_width(sTable, 1, 134);
  lv_timer_t* t = lv_timer_create(refresh, 1000, nullptr);
  // The timer dies with the screen, so a closed page costs nothing.
  lv_obj_add_event_cb(lv_obj_get_parent(lv_obj_get_parent(body)),
                      [](lv_event_t* e) { lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e)); },
                      LV_EVENT_DELETE, t);
  refresh(nullptr);
  push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

} // namespace Ui

#endif // HAS_LVGL_UI && HAS_GPS
