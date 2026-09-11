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

#include "UartLink.h"

#if HAS_SERIAL_LINK

#include <Arduino.h>
#include <HardwareSerial.h>
#include <atomic>
#include <new>
#include "HDLC.h"
#include "LocalLink.h"
#include "Diag.h"
#include "Watchdog.h"
#include "RingDrain.h"
#include "UartPortPolicy.h"
#include "Gps.h"

namespace UartLink {
namespace {

HardwareSerial sPort(BOARD_SERIAL_UART);
HDLC::Deframer sDeframer;
Uart::TxQueue  sQueue;

// The queue's memory, held only while the link is up. Plain pointers rather
// than a container: this is freed on end() and a container that reallocated
// would defeat the point.
uint8_t*             sArena = nullptr;
Uart::TxQueue::Slot* sSlots = nullptr;

portMUX_TYPE sLock = portMUX_INITIALIZER_UNLOCKED;

std::atomic<bool>     sUp{false};
std::atomic<bool>     sStop{false};
std::atomic<bool>     sTaskAlive{false};
std::atomic<uint32_t> sBaud{0};
std::atomic<uint32_t> sFramesRx{0}, sBytesRx{0};
// The deframer's own drop count, mirrored where another task may read it.
// `counters()` is documented safe from any task, and reading the deframer's
// plain member while the reader writes it is a race whatever the width makes
// of it in practice — the same reasoning Diag gives for mirroring its fault
// counts rather than exporting the originals.
std::atomic<uint32_t> sOversized{0};
// Passes the guard caught. A sink that throws every frame would otherwise
// spin silently: caught, skipped, caught again, with nothing said.
std::atomic<uint32_t> sGuardCaught{0};

FrameSink sSink = nullptr;
void*     sCtx  = nullptr;

// How long the reader waits with nothing to do. Short enough that a queued
// frame does not sit behind an idle wire, long enough that an idle link is not
// a spin: the task feeds the watchdog on every pass either way.
constexpr uint32_t kIdleMs = 5;

// Bytes read between watchdog feeds — bytes, not frames, because that is where
// the counter below actually sits. The cadence itself is RingDrain's and is
// read from there rather than re-typed: that header says it is "the one
// definition of that cadence" and the snapshot walk already borrows it, so a
// third copy here is exactly the drift it was consolidated to prevent.

// What one pass of the reader may spend. A bound rather than a tuning knob —
// unbounded, a wire delivering continuously would hold this task for as long
// as the far end kept talking, which is the shape that took the Reticulum task
// past its watchdog (issue 47) before that drain was capped.
constexpr uint32_t kPassBudgetMs = 50;

// Passes that threw before the link says so, the same shape the sensors use
// for a part that has stopped answering: complain once at a threshold rather
// than on every occurrence.
constexpr uint32_t kComplainAfter = 3;

void deliver(const uint8_t* frame, size_t len) {
  sFramesRx.fetch_add(1, std::memory_order_relaxed);
  if (sSink) sSink(frame, len, sCtx);
}

void readerTask(void*) {
  Watchdog::watch();
  uint8_t out[RNS_MTU];
  while (!sStop.load(std::memory_order_relaxed)) {
    Watchdog::feed();
    // Guarded, as every task loop this firmware starts is (Diag.h). The sink
    // is the caller's — by design the serial RNS interface or the remote-radio
    // link, both of which allocate — so a std::bad_alloc out of it would
    // otherwise unwind through the deframer and out of this function, where
    // nothing catches it and Diag's terminate handler aborts the node. One bad
    // pass is skipped; the link survives.
    // Diag::guard returns false when it caught something. Counted and said
    // once at a threshold rather than every pass: a sink that throws on every
    // frame would otherwise be a silent spin — caught, skipped, caught again —
    // which looks exactly like a quiet link from every surface.
    const bool clean = Diag::guard("the serial task", [&out] {
      const uint32_t started = millis();
      size_t sinceFeed = 0;
      bool did = false;

      // Receive first: the wire does not wait, and a frame left in the UART's
      // ring while the transmit queue drains is a frame an overrun can eat.
      //
      // A chunk at a time, not a byte. Each available()/read() pair takes the
      // core's UART mutex, so per-byte reads pay two mutex acquisitions and a
      // full uart_read_bytes call for every octet — about six per cent of a
      // core at 115200 and far worse at the top of the ladder, which is the
      // cost the roadmap says to measure before promising 921600. PppUart's
      // reader takes 256 bytes a call for the same reason.
      sOversized.store(sDeframer.oversized(), std::memory_order_relaxed);
      uint8_t chunk[64];
      while ((uint32_t)(millis() - started) < kPassBudgetMs) {
        const size_t n = sPort.read(chunk, sizeof(chunk));
        if (n == 0) break;
        sBytesRx.fetch_add((uint32_t)n, std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++) {
          sDeframer.feed(chunk[i], [](const uint8_t* f, size_t len) { deliver(f, len); });
          if (++sinceFeed % Sys::RingDrain::kFeedEvery == 0) Watchdog::feed();
        }
        did = true;
      }

      // Then transmit, one frame per pass at most, so a full queue cannot hold
      // the reader off the wire indefinitely.
      size_t len = 0;
      bool have = false;
      taskENTER_CRITICAL(&sLock);
      have = sQueue.pop(out, sizeof(out), len);
      taskEXIT_CRITICAL(&sLock);
      if (have) {
        static uint8_t framed[HDLC::frameCapacity(RNS_MTU)];
        // Static rather than on the stack: a kilobyte of framing buffer beside
        // the 500-byte frame left too little of this task's stack for the sink
        // that runs on it. Safe because the port has exactly one writer, which
        // is this task, and the header says so.
        const size_t n = HDLC::frame(out, len, framed, sizeof(framed));
        if (n) sPort.write(framed, n);
        did = true;
      }

      // Always a yield, not only when idle. Relying on the wire to run dry is
      // an argument that holds at the one baud this board matrix allows and
      // stops holding at the top of the ladder; a tick costs nothing and
      // removes the dependency on that arithmetic.
      vTaskDelay(pdMS_TO_TICKS(did ? 1 : kIdleMs));
    });
    if (!clean) {
      const uint32_t n = sGuardCaught.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n == kComplainAfter)
        log_w("serial: %u passes running have thrown — the frame sink is "
              "failing, and the link is dropping what it reads",
              (unsigned)kComplainAfter);
      // A pass that threw did no yielding of its own, so it yields here: a
      // throw on the first byte every time would otherwise be a tight loop.
      vTaskDelay(pdMS_TO_TICKS(kIdleMs));
    }
  }
  // Before the task goes, and this is not optional: nothing in IDF clears a
  // watchdog subscription when its task exits, so an entry left behind can
  // never be fed again and panics the node one timeout later — and again after
  // every reboot (Watchdog.h says so, having been bitten). Every other
  // self-deleting task here does the same (PppUart.cpp, AutoInterface.cpp).
  Watchdog::unwatch();
  sTaskAlive.store(false, std::memory_order_relaxed);
  vTaskDelete(nullptr);
}

// The two ports this firmware can see an owner for. It cannot see a wire, so a
// board that names a port something external is using gets no warning from
// here — which is why choosing the port is a bench question (UartLink.h).
bool portIsTaken(int uart) {
  // The rule is UartPortPolicy's; this only supplies the board's facts. Keeping
  // the two apart is what lets the rule be tested at all — everything in this
  // file is behind HAS_SERIAL_LINK, which no board sets.
  const UartPort::Owners owners{
      BOARD_UART_INSTANCE,
#if HAS_GPS
      true, Gps::kUartInstance,
#else
      false, -1,
#endif
  };
  return UartPort::taken(uart, owners);
}

} // namespace

bool begin(uint32_t baud, FrameSink sink, void* ctx) {
  if (sUp.load(std::memory_order_relaxed)) {
    // Already up is only a no-op when nothing was asked to change. A settings
    // apply that moves the baud would otherwise be told it succeeded and
    // change nothing, with no line anywhere saying so.
    if (baud == sBaud.load(std::memory_order_relaxed) && sink == sSink && ctx == sCtx)
      return true;
    log_w("serial: already up at %u baud — end() first to change it",
          (unsigned)sBaud.load(std::memory_order_relaxed));
    return false;
  }
  if (sTaskAlive.load(std::memory_order_relaxed)) {
    log_w("serial: the previous reader has not stopped yet");
    return false;
  }
  if (sArena || sSlots) {
    // A previous end() timed out and never finished. Allocating again here is
    // what would leak the old arena, so the teardown is completed first.
    log_w("serial: a previous teardown did not finish — completing it");
    taskENTER_CRITICAL(&sLock);
    sQueue.detach();
    taskEXIT_CRITICAL(&sLock);
    delete[] sArena; sArena = nullptr;
    delete[] sSlots; sSlots = nullptr;
  }

  // LocalLink's predicate, whole, rather than its primitive reassembled here.
  //
  // And a borrowed ceiling, said plainly rather than dressed up:
  // `tested_max_baud` is a fact about the *bridge* UART — the CP2102 or CH9102
  // and the cable behind it — and this link is a different port on different
  // pins with no bridge chip on them at all. An earlier comment here called it
  // "the same question about the same board", which it is not: same board,
  // different wire. It is used anyway, deliberately, as the conservative
  // placeholder until a bench qualifies this wire and the registry gains a
  // field of its own. The visible consequence is that every board allows
  // exactly one speed today, so "each supported baud" in the roadmap's
  // acceptance means 115200 until that field exists.
  if (!LocalLink::pppBaudUsable(baud)) {
    log_w("serial: %u baud is not one this board has been qualified at "
          "(the bridge UART's ladder, borrowed — see UartLink.cpp)", (unsigned)baud);
    return false;
  }
  if (portIsTaken(BOARD_SERIAL_UART)) {
    log_e("serial: UART%d already belongs to the console, PPP or the GNSS "
          "receiver — a board must name a port none of them uses",
          (int)BOARD_SERIAL_UART);
    return false;
  }

  // Off means not allocated, so the memory is found here and given back in
  // end(). nothrow because a link that cannot be afforded must decline rather
  // than abort the node (Diag.h installs a terminate handler that aborts).
  sArena = new (std::nothrow) uint8_t[SERIAL_LINK_TX_BYTES];
  sSlots = new (std::nothrow) Uart::TxQueue::Slot[SERIAL_LINK_TX_FRAMES];
  if (!sArena || !sSlots) {
    delete[] sArena; sArena = nullptr;
    delete[] sSlots; sSlots = nullptr;
    log_w("serial: not enough memory for a %u-byte transmit queue — link not started",
          (unsigned)SERIAL_LINK_TX_BYTES);
    return false;
  }

  sSink = sink;
  sCtx  = ctx;
  sDeframer.reset();
  taskENTER_CRITICAL(&sLock);
  sQueue.attach(sArena, SERIAL_LINK_TX_BYTES, sSlots, SERIAL_LINK_TX_FRAMES);
  taskEXIT_CRITICAL(&sLock);

  sPort.setRxBufferSize(SERIAL_LINK_RX_RING);
  sPort.begin(baud, SERIAL_8N1, PIN_SERIAL_LINK_RX, PIN_SERIAL_LINK_TX);
  if (!sPort) {
    // HardwareSerial::begin returns void and answers available() with 0 for
    // ever when the driver did not install; operator bool is the only thing
    // that says so (the same check Gps.cpp makes, for the same reason).
    log_e("serial: UART%d would not start at %u baud", (int)BOARD_SERIAL_UART, (unsigned)baud);
    taskENTER_CRITICAL(&sLock);
    sQueue.detach();
    taskEXIT_CRITICAL(&sLock);
    delete[] sArena; sArena = nullptr;
    delete[] sSlots; sSlots = nullptr;
    return false;
  }

  sStop.store(false, std::memory_order_relaxed);
  sBaud.store(baud, std::memory_order_relaxed);
  // Set here, before the task can run and before end() can look, which is the
  // shape AutoInterface uses and says why. Set inside the task instead, a
  // begin() immediately followed by end() reads false, skips the wait, and
  // frees the arena under a task that is about to start.
  sTaskAlive.store(true, std::memory_order_relaxed);
  if (!Diag::startTask(readerTask, "serial", SERIAL_LINK_TASK_STACK, nullptr, 2, 0)) {
    sTaskAlive.store(false, std::memory_order_relaxed);
    log_e("serial: the reader task would not start — link not up");
    sPort.end();
    taskENTER_CRITICAL(&sLock);
    sQueue.detach();
    taskEXIT_CRITICAL(&sLock);
    delete[] sArena; sArena = nullptr;
    delete[] sSlots; sSlots = nullptr;
    return false;
  }
  sUp.store(true, std::memory_order_relaxed);
  log_i("serial: UART%d up at %u baud on rx=%d tx=%d, %u-frame queue",
        (int)BOARD_SERIAL_UART, (unsigned)baud, (int)PIN_SERIAL_LINK_RX,
        (int)PIN_SERIAL_LINK_TX, (unsigned)SERIAL_LINK_TX_FRAMES);
  return true;
}

bool end() {
  if (!sUp.load(std::memory_order_relaxed)) return true;
  // `sUp` stays true until the link really is down. Cleared here, a timeout
  // below would leave the node reporting a link that is switched off while its
  // task still runs and still owns the port — and once that task did exit,
  // `begin()` would see nothing in its way and allocate a second arena on top
  // of the first, leaking two kilobytes for every cycle.
  sStop.store(true, std::memory_order_relaxed);
  // The task deletes itself; wait for it rather than freeing the arena under
  // it, which is the one ordering here that can crash a node.
  for (int i = 0; i < 200 && sTaskAlive.load(std::memory_order_relaxed); i++)
    vTaskDelay(pdMS_TO_TICKS(5));
  if (sTaskAlive.load(std::memory_order_relaxed)) {
    // The port and the arena stay exactly where they are. Freeing them under a
    // live reader is the one ordering here that can crash a node, and saying
    // "released" afterwards would make the log a lie as well — the shape
    // AutoInterface::end() already uses when its task will not stop.
    // `sStop` is deliberately left set and `sUp` left true: the link still owns
    // its port and its memory, and calling end() again is the way out — the
    // retry finds the task gone and completes the teardown properly. Anything
    // else here either frees memory under a running task or abandons it.
    log_w("serial: the reader did not stop inside a second — the link still "
          "holds its port and queue; call end() again");
    return false;
  }
  sUp.store(false, std::memory_order_relaxed);
  sPort.end();
  taskENTER_CRITICAL(&sLock);
  sQueue.detach();
  taskEXIT_CRITICAL(&sLock);
  delete[] sArena; sArena = nullptr;
  delete[] sSlots; sSlots = nullptr;
  sSink = nullptr;
  sCtx = nullptr;
  sBaud.store(0, std::memory_order_relaxed);
  log_i("serial: UART%d released", (int)BOARD_SERIAL_UART);
  return true;
}

bool up() { return sUp.load(std::memory_order_relaxed); }
uint32_t baud() { return sBaud.load(std::memory_order_relaxed); }

bool send(const uint8_t* frame, size_t len) {
  if (!sUp.load(std::memory_order_relaxed)) return false;
  // Refused here rather than accepted and then discarded by the reader. The
  // arena holds four MTUs, so the queue alone would take a 2000-byte frame
  // happily and say true — and the reader pops into an RNS_MTU buffer and
  // drops anything longer, counting it as congestion. The caller would have
  // been told the frame was queued and never learn otherwise.
  if (len > RNS_MTU) {
    log_w("serial: a %u-byte frame is longer than the %u-byte MTU this link "
          "carries", (unsigned)len, (unsigned)RNS_MTU);
    return false;
  }
  taskENTER_CRITICAL(&sLock);
  const bool ok = sQueue.push(frame, len);
  taskEXIT_CRITICAL(&sLock);
  return ok;
}

Counters counters() {
  Counters c;
  taskENTER_CRITICAL(&sLock);
  c.tx = sQueue.counters();
  taskEXIT_CRITICAL(&sLock);
  c.framesReceived = sFramesRx.load(std::memory_order_relaxed);
  c.bytesReceived  = sBytesRx.load(std::memory_order_relaxed);
  c.oversized      = sOversized.load(std::memory_order_relaxed);
  return c;
}

const uint32_t* qualifiedBauds(size_t& count) {
  // LocalLink's, not a second copy. An earlier version built the same array
  // from the same macro here and the header claimed no rule was restated,
  // which was true of the predicate and not of the ladder it is asked about.
  return LocalLink::pppBauds(count);
}

} // namespace UartLink

#endif  // HAS_SERIAL_LINK
