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
//  Lock.h — hold a FreeRTOS mutex for a scope, and give it back on every exit
//
//  A take/give pair written out by hand is correct until something leaves the
//  scope another way, and since Diag::guard() there is another way: a throw.
//  A mutex leaked that way is worse than the crash it was meant to contain —
//  the task that leaked it deadlocks on its own non-recursive mutex when the
//  guard retries, and every other task that wants it waits on portMAX_DELAY
//  for ever. A node that hangs silently is harder to diagnose than one that
//  restarts, which is the opposite of what the guard was for.
//
//  So the mutexes that are held across work which can throw are taken through
//  this: the destructor runs while the exception unwinds, and the lock is
//  given back on the way past.
//
//  Non-recursive, like the mutexes it holds. Taking one twice on a task
//  deadlocks that task, exactly as the bare calls did.
// ============================================================================
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Sys {

class Lock {
public:
  // Waits for as long as the bare calls did. A null handle is not an error:
  // it is a subsystem whose lock has not been created yet, and callers
  // already had to cope with that.
  explicit Lock(SemaphoreHandle_t h) : _h(h) {
    if (_h) xSemaphoreTake(_h, portMAX_DELAY);
  }
  ~Lock() { if (_h) xSemaphoreGive(_h); }

  // Give it back early, where the original code did work after the critical
  // section that must not hold it — copying a table out and then sending on
  // it, for instance. Safe to call twice; the destructor then does nothing.
  void release() {
    if (_h) { xSemaphoreGive(_h); _h = nullptr; }
  }

  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

private:
  SemaphoreHandle_t _h;
};

} // namespace Sys
