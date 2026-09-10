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

// ClientAdmit: what a peer on :4242 costs when it is turned away. The property
// the fix is, and the one an ESP32 cannot be made to demonstrate on demand, is
// the accounting: every context that is allocated is either enrolled or given
// back, every client that is not accepted is refused, and a refusal is never a
// leak of either. So the policy is driven here through its real code with the
// failures injected — a null from `new (std::nothrow)`, a std::bad_alloc out
// of the list's own allocator, and a type that is neither — rather than by
// starving a board until a stranger connects at the wrong moment.
//
// The context is built around the real HDLC::Deframer the node embeds, so the
// allocator is genuinely asked for a deframer-sized block rather than for a
// stand-in; the fake counts what it was asked for and what is still live, and
// the assertions read those.
//
// What this suite does not cover, and cannot: that
// RetiTransportServer::begin() calls _clients.reserve(), that
// RetiTransportServer::onClient() calls admit() at all, and that it attaches
// nothing carrying the context until Accepted comes back. All of that lives in
// src/net/RetiTransportServer.cpp, which includes AsyncTCP.h and FreeRTOS
// headers that test/stubs/ does not have, so it cannot be compiled for the
// host. That wiring is verified by reading it, not by running this — which is
// why the ordering rule is written down in ClientAdmit.h as a contract rather
// than left to be inferred from the caller.
#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <new>
#include <vector>
#include "Config.h"                      // RNS_MAX_CLIENTS, RNS_MTU
#include "HDLC.h"                        // the real deframer the context embeds
#include "../../src/net/ClientAdmit.h"

using Net::ClientAdmit::Outcome;

// ---------------------------------------------------------------------------
// The context, at the size the node really asks for. ClientCtx in
// RetiTransportServer.h is an id, an AsyncClient* and an HDLC::Deframer; this
// is the same three, with the client a pointer to a stand-in that counts
// itself rather than one that is ever dereferenced.
//
// The class-level allocation function is what makes the size assertions worth
// anything: `bytes` is the figure the compiler computed for this type, not one
// the test worked out, so the test observes the ask instead of restating it.
// It is also where a starved board is simulated — a null answer rather than a
// throw, which is what `new (std::nothrow)` gives.
// ---------------------------------------------------------------------------
static size_t g_askedBytes = 0;      // what the last `new` asked for
static size_t g_liveCtxs   = 0;      // contexts allocated and not yet freed
static bool   g_allocFails = false;  // the heap has no block that big

// The other thing a refusal must not leak: the client the framework handed
// over. refuse() in RetiTransportServer.cpp does not delete it itself — it
// installs a disconnect callback that does and then closes the socket, and the
// close runs that callback — so the fakes below delete it in their refuse(),
// which is what that sequence amounts to. g_liveClients is then a reading
// rather than an inference: the assertions can watch the client go.
static size_t g_liveClients = 0;

struct FakeClient {
  FakeClient()  { g_liveClients++; }
  ~FakeClient() { g_liveClients--; }
};

struct FakeCtx {
  uint32_t       id;
  FakeClient*    client;
  HDLC::Deframer deframer;

  static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    g_askedBytes = bytes;
    if (g_allocFails) return nullptr;
    void* p = ::operator new(bytes, std::nothrow);
    if (p) g_liveCtxs++;
    return p;
  }
  static void operator delete(void* p) noexcept {
    if (p) g_liveCtxs--;
    ::operator delete(p);
  }
  // Pairs with the placement form, for a constructor that threw. None of these
  // members has a constructor that can, but an unpaired placement new is a leak
  // waiting for the day one of them grows one.
  static void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (p) g_liveCtxs--;
    ::operator delete(p, std::nothrow);
  }
};

// A type with nothing to do with std::exception, for the breadth of the catch
// in admit() rather than for any failure the node is expected to produce. The
// header says the catch is deliberately `catch (...)` and why: whatever comes
// out is handled identically, and a type it did not expect escaping onto a task
// with nothing above it to catch is the panic the whole unit exists to remove.
// A suite that only ever threw std::bad_alloc could not tell that catch from a
// narrow one — a `catch (const std::bad_alloc&)` in its place passed every case
// here — so one case throws this instead.
struct SomethingElse { int marker = 0xE5; };

// ---------------------------------------------------------------------------
// The listener's side of onClient(), with nothing real in it: the list is a
// count, the client is the stand-in above, and "refuse" frees it the way the
// close on the node does.
// ---------------------------------------------------------------------------
struct FakeListener {
  // The node's state on this attempt.
  bool   restarting  = false;      // Bootloader::pending()
  bool   enrolThrows = false;      // the push had to allocate, and could not
  bool   enrolThrowsUnrelated = false;  // ...or threw something else entirely
  size_t clients     = 0;          // clients already in the list

  // What the attempt cost.
  size_t restartAsks = 0;
  size_t countAsks   = 0;
  size_t allocated   = 0;
  size_t enrolled    = 0;
  size_t released    = 0;
  size_t refused     = 0;

  std::vector<FakeCtx*> accepted;   // contexts the caller owns from here on
  ~FakeListener() { for (auto* c : accepted) { delete c->client; delete c; } }

  // What the caller is left holding — onClient()'s local `ctx`, published from
  // the end of enrol() rather than captured out of the allocation. Modelled
  // here because the shape is the protection: on every outcome but Accepted the
  // context has already gone back to release(), so a caller that took the
  // pointer any earlier would be holding freed memory.
  FakeCtx* published = nullptr;

  Outcome admit(size_t cap) {
    published = nullptr;
    FakeClient* client = new FakeClient();     // what the framework hands over
    return Net::ClientAdmit::admit(
        cap,
        [this] { restartAsks++; return restarting; },
        [this] { countAsks++;   return clients; },
        [this, client]() -> FakeCtx* {
          FakeCtx* ctx = new (std::nothrow) FakeCtx{ 1, client, {} };
          if (ctx) allocated++;
          return ctx;
        },
        [this](FakeCtx* ctx) {
          if (enrolThrowsUnrelated) throw SomethingElse{};
          if (enrolThrows) throw std::bad_alloc();
          accepted.push_back(ctx);
          enrolled++;
          clients++;
          published = ctx;              // last, as the contract requires
        },
        [this](FakeCtx* ctx) { released++; delete ctx; },
        [this, client] { refused++; delete client; });
  }
};

// ---------------------------------------------------------------------------
// The list itself, for the two cases that are about the list rather than about
// the policy: a real std::vector under an allocator that can be made to fail
// the way a starved board's does — by throwing std::bad_alloc out of
// allocate(), which is what ::operator new does. Nothing else about it differs
// from std::allocator, so the growth behaviour is the firmware's.
// ---------------------------------------------------------------------------
static size_t g_vecAllocs     = 0;
static bool   g_vecAllocFails = false;

template <typename T>
struct Failable {
  using value_type = T;
  Failable() = default;
  template <typename U> Failable(const Failable<U>&) {}
  T* allocate(size_t n) {
    g_vecAllocs++;
    if (g_vecAllocFails) throw std::bad_alloc();
    return static_cast<T*>(::operator new(n * sizeof(T)));
  }
  void deallocate(T* p, size_t) { ::operator delete(p); }
  template <typename U> bool operator==(const Failable<U>&) const { return true; }
  template <typename U> bool operator!=(const Failable<U>&) const { return false; }
};

using Slots = std::vector<FakeCtx*, Failable<FakeCtx*>>;

// The same six callables, with enrol() doing the push the node does — so the
// throw comes out of the container rather than out of a flag.
struct ListListener {
  explicit ListListener(Slots& l) : list(l) {}
  Slots& list;
  size_t allocated = 0, enrolled = 0, released = 0, refused = 0;

  Outcome admit(size_t cap) {
    FakeClient* client = new FakeClient();
    return Net::ClientAdmit::admit(
        cap,
        [] { return false; },
        [this] { return list.size(); },
        [this, client]() -> FakeCtx* {
          FakeCtx* ctx = new (std::nothrow) FakeCtx{ 1, client, {} };
          if (ctx) allocated++;
          return ctx;
        },
        [this](FakeCtx* ctx) { list.push_back(ctx); enrolled++; },
        [this](FakeCtx* ctx) { released++; delete ctx; },
        [this, client] { refused++; delete client; });
  }
};

static void freeAll(Slots& list) {
  for (auto* c : list) { delete c->client; delete c; }
  list.clear();
}

// ---------------------------------------------------------------------------

// Both arms of the cap, so retuning either has to come through this test.
//
// Asserting `BOARD_DRAM_TIGHT ? 2 : 4` against RNS_MAX_CLIENTS would pin only
// one of them: the flag is a compile-time constant, so on the host both sides
// of that conditional collapse to the same arm and the other number is held by
// nothing. The arm nobody compiles here is the one that matters most — it is
// the no-PSRAM board's.
//
// So the macro is expanded twice with the flag forced each way. RNS_MAX_CLIENTS
// is defined as a conditional *over* BOARD_DRAM_TIGHT, so the expansion picks
// up whichever value is in force at the point of use; push_macro/pop_macro put
// the build's own value back exactly, without this file having to know what it
// was. If the definition in Config.h ever stops being a conditional the two
// constants below come out equal and the assertions say so.
#pragma push_macro("BOARD_DRAM_TIGHT")
#undef BOARD_DRAM_TIGHT
#define BOARD_DRAM_TIGHT 1
static constexpr int kTightArmCap = RNS_MAX_CLIENTS;
#undef BOARD_DRAM_TIGHT
#define BOARD_DRAM_TIGHT 0
static constexpr int kRoomyArmCap = RNS_MAX_CLIENTS;
#pragma pop_macro("BOARD_DRAM_TIGHT")

static void test_both_arms_of_the_cap_are_what_config_says() {
  TEST_ASSERT_EQUAL_INT(2, kTightArmCap);
  TEST_ASSERT_EQUAL_INT(4, kRoomyArmCap);

  // BOARD_DRAM_TIGHT is the ESP32-without-PSRAM class (Config.h) and the host
  // is not in it, so every case below runs against the roomy arm. The tight
  // arm is only ever compiled by heltec-ws — the one env in platformio.ini on
  // classic ESP32 silicon with no -DBOARD_HAS_PSRAM — which is why that board
  // is built for this change.
  TEST_ASSERT_EQUAL_INT(0, BOARD_DRAM_TIGHT);
  TEST_ASSERT_EQUAL_INT(kRoomyArmCap, RNS_MAX_CLIENTS);
}

// The allocation this unit exists for: a context that carries a whole
// deframer, in one contiguous block.
static void test_the_context_asked_for_carries_a_whole_deframer() {
  // RNS.Reticulum's MTU, which is what the deframer's buffer is sized from
  // (src/Config.h:395). The deframer is that buffer plus its state, so a
  // context is an MTU's worth of bytes and then some — never less, on any
  // word size.
  TEST_ASSERT_EQUAL_size_t(500, (size_t)RNS_MTU);
  TEST_ASSERT_GREATER_OR_EQUAL_size_t((size_t)RNS_MTU, sizeof(HDLC::Deframer));

  FakeListener listener;
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)listener.admit(RNS_MAX_CLIENTS));
  // What `new` was asked for, reported by the allocation function from the
  // compiler's own figure: the context, deframer and all.
  TEST_ASSERT_EQUAL_size_t(sizeof(FakeCtx), g_askedBytes);
  TEST_ASSERT_GREATER_THAN_size_t(sizeof(HDLC::Deframer), g_askedBytes);
}

// The first question, and the only one that costs nothing at all.
static void test_a_pending_restart_refuses_before_the_cap_is_even_read() {
  FakeListener listener;
  listener.restarting = true;
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedRestarting,
                        (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_EQUAL_size_t(1, listener.restartAsks);
  // Not even the list is consulted, so on the node its lock is never taken.
  TEST_ASSERT_EQUAL_size_t(0, listener.countAsks);
  TEST_ASSERT_EQUAL_size_t(0, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);      // and the socket went with it
}

// The ordering the reserve in begin() depends on: at the cap, the context is
// never made, so the list is never asked to hold a cap+1'th.
static void test_the_cap_refuses_before_anything_is_allocated() {
  FakeListener listener;
  listener.clients = (size_t)RNS_MAX_CLIENTS;
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedFull,
                        (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_EQUAL_size_t(1, listener.countAsks);
  TEST_ASSERT_EQUAL_size_t(0, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);
  TEST_ASSERT_EQUAL_size_t(0, g_askedBytes);      // `new` was never reached
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);
}

static void test_an_accepted_client_is_enrolled_once_and_never_released() {
  FakeListener listener;
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_EQUAL_size_t(1, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(1, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(0, listener.released);
  TEST_ASSERT_EQUAL_size_t(0, listener.refused);
  TEST_ASSERT_EQUAL_size_t(1, listener.clients);
  // The context is the caller's from here — it is what the callbacks it is
  // about to attach will carry — so it is still live, and freeing it is the
  // disconnect path's job rather than this one's. So is the client: an accepted
  // one is not closed, which is the difference this counter reads.
  TEST_ASSERT_EQUAL_size_t(1, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(1, g_liveClients);
}

// The starved board's answer to `new (std::nothrow)`: a null pointer instead
// of the std::bad_alloc that would reach the terminate handler and abort.
static void test_a_failed_allocation_refuses_the_client_and_frees_nothing_it_did_not_take() {
  FakeListener listener;
  g_allocFails = true;
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedNoMemory,
                        (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);
  TEST_ASSERT_EQUAL_size_t(0, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(0, listener.released);   // there was nothing to give back
  TEST_ASSERT_EQUAL_size_t(0, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(0, listener.clients);
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);
  // The ask still happened, at the full size: this is the contiguous block a
  // no-PSRAM board cannot always find.
  TEST_ASSERT_EQUAL_size_t(sizeof(FakeCtx), g_askedBytes);
}

// The case the whole exercise is about. The context exists, the list will not
// take it, and the throw must stop here: on the node there is nothing above
// this on the AsyncTCP event task to catch one — Diag::begin() installs
// std::set_terminate (src/sys/Diag.cpp:230) and that handler ends in abort()
// (:212), so an escape is a panic rather than a refused client.
static void test_an_enrolment_that_throws_frees_the_context_and_refuses_the_client() {
  FakeListener listener;
  listener.enrolThrows = true;
  Outcome outcome = Outcome::Accepted;
  bool escaped = false;
  try {
    outcome = listener.admit(RNS_MAX_CLIENTS);
  } catch (...) {
    escaped = true;
  }
  TEST_ASSERT_FALSE(escaped);
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedNotEnrolled, (int)outcome);
  TEST_ASSERT_EQUAL_size_t(1, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(1, listener.released);   // and it went back
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);    // and so did the client
  TEST_ASSERT_EQUAL_size_t(0, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(0, listener.clients);
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);      // closed and freed, not held
}

// The same accounting when what came out of enrol() is not a std::bad_alloc,
// and not even a std::exception. Nothing in the node is expected to throw a
// SomethingElse; the point is that admit() does not get to depend on that.
// Whatever a future enrol() — or a library underneath it — lets out, the
// context still goes back, the client is still refused, and nothing reaches the
// terminate handler. This is the case that holds the `catch (...)` in place: a
// `catch (const std::bad_alloc&)` in its stead leaves this one escaping.
static void test_an_enrolment_that_throws_anything_at_all_is_still_contained() {
  FakeListener listener;
  listener.enrolThrowsUnrelated = true;
  Outcome outcome = Outcome::Accepted;
  bool escaped = false;
  try {
    outcome = listener.admit(RNS_MAX_CLIENTS);
  } catch (...) {
    escaped = true;
  }
  TEST_ASSERT_FALSE(escaped);                       // nothing came past admit()
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedNotEnrolled, (int)outcome);
  TEST_ASSERT_EQUAL_size_t(1, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(1, listener.released);   // the context went back
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);    // and the client was closed
  TEST_ASSERT_EQUAL_size_t(0, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(0, listener.clients);
  TEST_ASSERT_NULL(listener.published);             // and the caller holds nothing
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);
}

// The caller's half of the accounting, which the header states and cannot
// enforce: a pointer published from the end of enrol() exists only where the
// context does. On every refusal the caller is left holding nothing, so the
// log line that names the peer has no freed context to reach through — which is
// the failure this shape exists to make unwritable.
static void test_a_refusal_leaves_the_caller_no_pointer_at_all() {
  {
    FakeListener listener;
    TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)listener.admit(RNS_MAX_CLIENTS));
    TEST_ASSERT_NOT_NULL(listener.published);
    TEST_ASSERT_EQUAL_PTR(listener.accepted.back(), listener.published);
  }

  { FakeListener l; l.restarting = true;
    TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedRestarting, (int)l.admit(RNS_MAX_CLIENTS));
    TEST_ASSERT_NULL(l.published); }

  { FakeListener l; l.clients = (size_t)RNS_MAX_CLIENTS;
    TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedFull, (int)l.admit(RNS_MAX_CLIENTS));
    TEST_ASSERT_NULL(l.published); }

  { FakeListener l; g_allocFails = true;
    TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedNoMemory, (int)l.admit(RNS_MAX_CLIENTS));
    TEST_ASSERT_NULL(l.published);
    g_allocFails = false; }

  // The one that matters most: here the context did exist, and was handed back
  // to release() inside admit(). Nothing was published, so nothing outlives it.
  { FakeListener l; l.enrolThrows = true;
    TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedNotEnrolled, (int)l.admit(RNS_MAX_CLIENTS));
    TEST_ASSERT_NULL(l.published); }

  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);
}

// A node under real memory pressure does not fail once. It fails for every
// peer that tries, for as long as the pressure lasts, and what must not happen
// across that run is any accumulation at all.
static void test_nothing_leaks_across_a_run_of_failures() {
  FakeListener listener;
  const int kAttempts = 50;
  for (int i = 0; i < kAttempts; i++) {
    const bool starved = (i % 2 == 0);
    g_allocFails         = starved;        // the context could not be made
    listener.enrolThrows = !starved;       // the list could not grow
    const Outcome outcome = listener.admit(RNS_MAX_CLIENTS);
    TEST_ASSERT_EQUAL_INT((int)(starved ? Outcome::RefusedNoMemory
                                        : Outcome::RefusedNotEnrolled),
                          (int)outcome);
    TEST_ASSERT_EQUAL_size_t(0, listener.clients);   // the count never moves
    TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);         // nothing is held over
    TEST_ASSERT_EQUAL_size_t(0, g_liveClients);      // of either kind
  }
  TEST_ASSERT_EQUAL_size_t(kAttempts, listener.refused);
  TEST_ASSERT_EQUAL_size_t(kAttempts / 2, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(listener.allocated, listener.released);
  TEST_ASSERT_EQUAL_size_t(0, listener.enrolled);
}

// What the reserve in begin() buys, read off the real container: with the room
// already taken, admitting every client this node will hold costs the list no
// allocation, so the push cannot throw and every attempt is Accepted.
static void test_a_reserved_list_admits_the_cap_without_allocating() {
  Slots list;
  list.reserve(RNS_MAX_CLIENTS);
  TEST_ASSERT_EQUAL_size_t(1, g_vecAllocs);
  TEST_ASSERT_GREATER_OR_EQUAL_size_t((size_t)RNS_MAX_CLIENTS, list.capacity());

  // From here the list may not allocate at all: anything it asks for throws.
  // If a push needed memory, the admissions below would come back refused.
  g_vecAllocFails = true;
  ListListener listener(list);
  for (int i = 0; i < RNS_MAX_CLIENTS; i++)
    TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)listener.admit(RNS_MAX_CLIENTS));

  TEST_ASSERT_EQUAL_size_t((size_t)RNS_MAX_CLIENTS, list.size());
  TEST_ASSERT_EQUAL_size_t(1, g_vecAllocs);        // still just the reserve
  TEST_ASSERT_EQUAL_size_t((size_t)RNS_MAX_CLIENTS, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(0, listener.released);
  TEST_ASSERT_EQUAL_size_t(0, listener.refused);
  // One client per accepted context and not one closed: nothing has been
  // refused yet, so none of them has been through refuse().
  TEST_ASSERT_EQUAL_size_t((size_t)RNS_MAX_CLIENTS, g_liveClients);

  // ...and the one past the cap is refused by the cap rather than by the
  // allocator: nothing is asked for on its behalf, from either heap.
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedFull, (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);
  TEST_ASSERT_EQUAL_size_t(1, g_vecAllocs);
  TEST_ASSERT_EQUAL_size_t((size_t)RNS_MAX_CLIENTS, listener.allocated);
  // ...and the one turned away took its client with it, leaving the accepted
  // ones exactly as they were.
  TEST_ASSERT_EQUAL_size_t((size_t)RNS_MAX_CLIENTS, g_liveClients);

  freeAll(list);
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);
}

// And what a board that could not satisfy the reserve is left with — the case
// that makes the catch in admit() reachable rather than decorative. The
// equivalence between "reserve was never called" and "reserve was called, threw
// and was caught" is reserve()'s strong guarantee; it is read here rather than
// asserted in prose.
static void test_a_failed_reserve_leaves_a_push_that_can_throw_and_is_caught() {
  Slots list;
  g_vecAllocFails = true;
  bool reserveThrew = false;
  try {
    list.reserve(RNS_MAX_CLIENTS);      // begin()'s reserve, inside Diag::guard
  } catch (const std::bad_alloc&) {
    reserveThrew = true;
  }
  TEST_ASSERT_TRUE(reserveThrew);
  TEST_ASSERT_EQUAL_size_t(0, list.capacity());     // left exactly as it was
  TEST_ASSERT_EQUAL_size_t(0, list.size());

  // So the first client to arrive makes the push allocate, and on this board
  // the allocation fails. That throw is caught inside admit(): the context goes
  // back, the client is refused, and the list is untouched.
  ListListener listener(list);
  TEST_ASSERT_EQUAL_INT((int)Outcome::RefusedNotEnrolled,
                        (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_GREATER_OR_EQUAL_size_t(2, g_vecAllocs);   // the reserve, then the push
  TEST_ASSERT_EQUAL_size_t(1, listener.allocated);
  TEST_ASSERT_EQUAL_size_t(1, listener.released);
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);
  TEST_ASSERT_EQUAL_size_t(0, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(0, list.size());
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);

  // The refusal is per attempt and not a latch: when there is memory again,
  // the same list takes the next client.
  g_vecAllocFails = false;
  TEST_ASSERT_EQUAL_INT((int)Outcome::Accepted, (int)listener.admit(RNS_MAX_CLIENTS));
  TEST_ASSERT_EQUAL_size_t(1, list.size());
  TEST_ASSERT_EQUAL_size_t(1, listener.enrolled);
  TEST_ASSERT_EQUAL_size_t(1, listener.refused);
  TEST_ASSERT_EQUAL_size_t(1, g_liveClients);

  freeAll(list);
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);
}

void setUp() {
  // Read before they are cleared. Zeroing first would let a context or a client
  // leaked by one case be swallowed by the next, which is the one kind of
  // failure this suite exists to catch.
  TEST_ASSERT_EQUAL_size_t(0, g_liveCtxs);
  TEST_ASSERT_EQUAL_size_t(0, g_liveClients);

  g_askedBytes    = 0;
  g_liveCtxs      = 0;
  g_liveClients   = 0;
  g_allocFails    = false;
  g_vecAllocs     = 0;
  g_vecAllocFails = false;
}

void tearDown() {
  // Cleared here as well, so a case that fails part-way cannot leave the next
  // one running against an allocator that refuses everything.
  g_allocFails    = false;
  g_vecAllocFails = false;
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_both_arms_of_the_cap_are_what_config_says);
  RUN_TEST(test_the_context_asked_for_carries_a_whole_deframer);
  RUN_TEST(test_a_pending_restart_refuses_before_the_cap_is_even_read);
  RUN_TEST(test_the_cap_refuses_before_anything_is_allocated);
  RUN_TEST(test_an_accepted_client_is_enrolled_once_and_never_released);
  RUN_TEST(test_a_failed_allocation_refuses_the_client_and_frees_nothing_it_did_not_take);
  RUN_TEST(test_an_enrolment_that_throws_frees_the_context_and_refuses_the_client);
  RUN_TEST(test_an_enrolment_that_throws_anything_at_all_is_still_contained);
  RUN_TEST(test_a_refusal_leaves_the_caller_no_pointer_at_all);
  RUN_TEST(test_nothing_leaks_across_a_run_of_failures);
  RUN_TEST(test_a_reserved_list_admits_the_cap_without_allocating);
  RUN_TEST(test_a_failed_reserve_leaves_a_push_that_can_throw_and_is_caught);
  return UNITY_END();
}
