// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The local-link phase machine and the trust rule the bootloader policy
// rests on. Every link driver — Wi-Fi today, USB and PPP later — feeds the
// same machine, so the machine is the thing to test: a driver that reports
// "ready" with no address, or that keeps an address after losing carrier,
// would be a bug in every link at once.

#include <unity.h>
#include "LocalLinkState.h"
#include "LazyStart.h"

using namespace LocalLink;

static void test_a_disabled_link_ignores_carrier_and_address() {
  Machine m(false);
  TEST_ASSERT_EQUAL((int)Phase::Disabled, (int)m.phase());
  TEST_ASSERT_FALSE(m.apply(Event::CarrierUp, 0));
  TEST_ASSERT_FALSE(m.apply(Event::AddressUp, 0));
  TEST_ASSERT_EQUAL((int)Phase::Disabled, (int)m.phase());
}

static void test_enable_carrier_address_reach_ready_in_order() {
  Machine m(false);
  TEST_ASSERT_TRUE(m.apply(Event::Enable, 0));
  TEST_ASSERT_EQUAL((int)Phase::Down, (int)m.phase());
  TEST_ASSERT_TRUE(m.apply(Event::CarrierUp, 100));
  TEST_ASSERT_EQUAL((int)Phase::Up, (int)m.phase());
  TEST_ASSERT_TRUE(m.apply(Event::AddressUp, 200));
  TEST_ASSERT_EQUAL((int)Phase::Ready, (int)m.phase());
}

static void test_a_static_address_can_arrive_with_the_carrier() {
  // A USB link with a fixed address is ready the moment the host attaches;
  // there is no separate "up, waiting for an address" moment to insist on.
  Machine m(true);
  TEST_ASSERT_TRUE(m.apply(Event::AddressUp, 0));
  TEST_ASSERT_EQUAL((int)Phase::Ready, (int)m.phase());
}

static void test_losing_carrier_loses_the_address() {
  Machine m(true);
  m.apply(Event::CarrierUp, 0);
  m.apply(Event::AddressUp, 0);
  TEST_ASSERT_TRUE(m.apply(Event::CarrierDown, 5000));
  TEST_ASSERT_EQUAL((int)Phase::Down, (int)m.phase());
  // ...and the uptime with it.
  TEST_ASSERT_EQUAL_UINT32(0, m.uptimeS(9000));
}

static void test_losing_the_address_keeps_the_carrier() {
  Machine m(true);
  m.apply(Event::CarrierUp, 0);
  m.apply(Event::AddressUp, 0);
  TEST_ASSERT_TRUE(m.apply(Event::AddressDown, 100));
  TEST_ASSERT_EQUAL((int)Phase::Up, (int)m.phase());
}

static void test_uptime_counts_from_becoming_ready() {
  Machine m(true);
  m.apply(Event::CarrierUp, 1000);
  TEST_ASSERT_EQUAL_UINT32(0, m.uptimeS(4000));         // up is not reachable
  m.apply(Event::AddressUp, 4000);
  TEST_ASSERT_EQUAL_UINT32(6, m.uptimeS(10500));
  // Re-entering Ready restarts the clock; a link that flapped is not one that
  // has been reachable all along.
  m.apply(Event::CarrierDown, 11000);
  m.apply(Event::AddressUp, 12000);
  TEST_ASSERT_EQUAL_UINT32(1, m.uptimeS(13000));
}

static void test_disable_works_from_every_phase() {
  for (int start = 0; start < 3; start++) {
    Machine m(true);
    if (start >= 1) m.apply(Event::CarrierUp, 0);
    if (start >= 2) m.apply(Event::AddressUp, 0);
    TEST_ASSERT_TRUE(m.apply(Event::Disable, 0));
    TEST_ASSERT_EQUAL((int)Phase::Disabled, (int)m.phase());
  }
}

static void test_repeated_events_do_not_report_a_change() {
  Machine m(true);
  TEST_ASSERT_TRUE(m.apply(Event::CarrierUp, 0));
  TEST_ASSERT_FALSE(m.apply(Event::CarrierUp, 0));
  TEST_ASSERT_TRUE(m.apply(Event::AddressUp, 0));
  TEST_ASSERT_FALSE(m.apply(Event::AddressUp, 0));
  TEST_ASSERT_FALSE(m.apply(Event::Enable, 0));
}

// --- names: the API vocabulary must never print "unknown" for a real value --

static void test_every_type_and_phase_has_a_name() {
  const Type types[] = { Type::WifiAp, Type::WifiSta, Type::UsbNcm, Type::PppUart };
  for (Type t : types) TEST_ASSERT_NOT_EQUAL(0, strcmp(typeName(t), "unknown"));
  const Phase phases[] = { Phase::Disabled, Phase::Down, Phase::Up, Phase::Ready };
  for (Phase p : phases) TEST_ASSERT_NOT_EQUAL(0, strcmp(phaseName(p), "unknown"));
  TEST_ASSERT_EQUAL_STRING("usb_ncm", typeName(Type::UsbNcm));
  TEST_ASSERT_EQUAL_STRING("ppp_uart", typeName(Type::PppUart));
}

static void test_only_the_station_uplink_is_not_host_facing() {
  // The bootloader API trusts host-facing links by default. The station link
  // is somebody else's LAN, which is exactly the network a deployed relay must
  // not take a reboot-into-downloader from.
  TEST_ASSERT_TRUE(isHostFacing(Type::WifiAp));
  TEST_ASSERT_TRUE(isHostFacing(Type::UsbNcm));
  TEST_ASSERT_TRUE(isHostFacing(Type::PppUart));
  TEST_ASSERT_FALSE(isHostFacing(Type::WifiSta));
}

static void test_the_usb_link_takes_its_subnet_from_the_mac() {
  // Two nodes on one computer: two subnets, decided by a byte nobody typed.
  TEST_ASSERT_EQUAL_UINT32(ipv4(10, 64, 0x54, 1), usbNodeAddress(0x54));
  TEST_ASSERT_EQUAL_UINT32(ipv4(10, 64, 0x54, 2), usbHostAddress(0x54));
  TEST_ASSERT_NOT_EQUAL(usbNodeAddress(0x54), usbNodeAddress(0x55));
  // The two ends share the subnet and nothing else.
  TEST_ASSERT_EQUAL_UINT32(usbNodeAddress(7) & kUsbNetmask, usbHostAddress(7) & kUsbNetmask);
  TEST_ASSERT_NOT_EQUAL(usbNodeAddress(7), usbHostAddress(7));
  TEST_ASSERT_TRUE(isHostFacing(Type::UsbNcm));
}

static void test_the_ppp_link_takes_the_next_subnet_over_from_the_same_byte() {
  // 10.65.<n>: the USB rule one subnet over, so a node on both links at
  // once — a Heltec on a bridge and a T3-S3 on its own USB, on one desk —
  // never lands two links on one subnet. The node asks for .1 and the host
  // is told to take .2; the peer decides, since the node is the client.
  TEST_ASSERT_EQUAL_UINT32(ipv4(10, 65, 0x54, 1), pppNodeAddress(0x54));
  TEST_ASSERT_EQUAL_UINT32(ipv4(10, 65, 0x54, 2), pppHostAddress(0x54));
  TEST_ASSERT_NOT_EQUAL(pppNodeAddress(0x54), usbNodeAddress(0x54));
  TEST_ASSERT_NOT_EQUAL(pppNodeAddress(0x54), pppNodeAddress(0x55));
  TEST_ASSERT_TRUE(isHostFacing(Type::PppUart));
  TEST_ASSERT_EQUAL_STRING("ipcp", addressingName(Addressing::Ipcp));
}

static void test_a_ppp_baud_must_be_listed_and_no_faster_than_tried() {
  // The ladder comes from boards.json; the ceiling is what the board has
  // actually been run at. Both bind: 921600 is on every board's ladder and
  // refused on every board until somebody has tried it.
  const uint32_t ladder[] = { 115200, 230400, 460800, 921600 };
  const size_t n = sizeof(ladder) / sizeof(ladder[0]);
  TEST_ASSERT_TRUE(pppBaudAllowed(115200, ladder, n, 115200));
  TEST_ASSERT_FALSE(pppBaudAllowed(230400, ladder, n, 115200));
  TEST_ASSERT_TRUE(pppBaudAllowed(230400, ladder, n, 460800));
  TEST_ASSERT_TRUE(pppBaudAllowed(460800, ladder, n, 460800));
  TEST_ASSERT_FALSE(pppBaudAllowed(921600, ladder, n, 460800));
  // Not on the ladder at all: neither a typo nor a rate the bridge could
  // do but nobody qualified.
  TEST_ASSERT_FALSE(pppBaudAllowed(9600, ladder, n, 921600));
  TEST_ASSERT_FALSE(pppBaudAllowed(250000, ladder, n, 921600));
  TEST_ASSERT_FALSE(pppBaudAllowed(0, ladder, n, 921600));
  // A board with one rung offers exactly that.
  const uint32_t one[] = { 115200 };
  TEST_ASSERT_TRUE(pppBaudAllowed(115200, one, 1, 115200));
  TEST_ASSERT_FALSE(pppBaudAllowed(230400, one, 1, 230400));
}

static void test_an_address_is_one_number_octets_first() {
  // hostOrder() and the trust rule compare these; the first octet has to be
  // the most significant or two addresses on one link compare unequal.
  TEST_ASSERT_EQUAL_UINT32(0x0A2A0001u, ipv4(10, 42, 0, 1));
  TEST_ASSERT_NOT_EQUAL(ipv4(10, 42, 0, 1), ipv4(1, 0, 42, 10));
}

// --- no way in --------------------------------------------------------------
static void test_the_console_alone_is_a_way_in() {
  // It is on every board and it carries the network console behind it, so
  // with it on nothing else can lock the operator out.
  TEST_ASSERT_FALSE(wouldLockOut(false, true, false));   // no link, no portal
  TEST_ASSERT_FALSE(wouldLockOut(true, true, true));
}

static void test_with_the_console_off_a_link_needs_something_listening_on_it() {
  // A link switched on is not a way in by itself: the portal is what answers
  // on it once the console — and the listener behind it — is off.
  TEST_ASSERT_TRUE(wouldLockOut(true, false, false));    // a link, nothing serving
  TEST_ASSERT_FALSE(wouldLockOut(true, false, true));    // a link and the portal
}

static void test_with_the_console_off_and_no_link_there_is_no_way_in() {
  TEST_ASSERT_TRUE(wouldLockOut(false, false, true));    // a portal nothing can reach
  TEST_ASSERT_TRUE(wouldLockOut(false, false, false));
}

// --- when a link that is built on demand may be built ------------------------
static void test_a_build_that_failed_is_not_retried_every_pass() {
  // poll() runs hundreds of times a second. A node out of byte-addressable
  // RAM would otherwise bury the heap figures an operator needs under
  // thousands of copies of its own complaint about them.
  LocalLink::LazyStart z;
  TEST_ASSERT_TRUE(z.shouldStart(true, false, false));
  z.built(false);
  TEST_ASSERT_FALSE(z.shouldStart(true, false, false));
  TEST_ASSERT_TRUE(z.failed());
}

static void test_the_switch_going_off_offers_another_try() {
  // The only gesture an operator has for "try again".
  LocalLink::LazyStart z;
  z.built(false);
  z.idle(true, false, false);                  // still on: the latch holds
  TEST_ASSERT_FALSE(z.shouldStart(true, false, false));
  z.idle(false, false, false);                 // off, and nothing left to give back
  TEST_ASSERT_FALSE(z.failed());
  TEST_ASSERT_TRUE(z.shouldStart(true, false, false));
}

static void test_nothing_is_built_or_torn_down_while_a_teardown_runs() {
  // A switch flicked off and straight back on would otherwise leave two
  // interfaces, or none, depending on the pass it landed in.
  LocalLink::LazyStart z;
  TEST_ASSERT_FALSE(z.shouldStart(true, false, true));
  TEST_ASSERT_FALSE(z.shouldStop(false, true, true));
  TEST_ASSERT_TRUE(z.shouldStop(false, true, false));
}

static void test_a_build_that_worked_clears_an_earlier_failure() {
  LocalLink::LazyStart z;
  z.built(false);
  z.built(true);
  TEST_ASSERT_FALSE(z.failed());
}

static void test_an_existing_link_is_not_built_again() {
  LocalLink::LazyStart z;
  TEST_ASSERT_FALSE(z.shouldStart(true, true, false));
  TEST_ASSERT_FALSE(z.shouldStop(true, true, false));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_build_that_failed_is_not_retried_every_pass);
  RUN_TEST(test_the_switch_going_off_offers_another_try);
  RUN_TEST(test_nothing_is_built_or_torn_down_while_a_teardown_runs);
  RUN_TEST(test_a_build_that_worked_clears_an_earlier_failure);
  RUN_TEST(test_an_existing_link_is_not_built_again);
  RUN_TEST(test_the_console_alone_is_a_way_in);
  RUN_TEST(test_with_the_console_off_a_link_needs_something_listening_on_it);
  RUN_TEST(test_with_the_console_off_and_no_link_there_is_no_way_in);
  RUN_TEST(test_a_disabled_link_ignores_carrier_and_address);
  RUN_TEST(test_enable_carrier_address_reach_ready_in_order);
  RUN_TEST(test_a_static_address_can_arrive_with_the_carrier);
  RUN_TEST(test_losing_carrier_loses_the_address);
  RUN_TEST(test_losing_the_address_keeps_the_carrier);
  RUN_TEST(test_uptime_counts_from_becoming_ready);
  RUN_TEST(test_disable_works_from_every_phase);
  RUN_TEST(test_repeated_events_do_not_report_a_change);
  RUN_TEST(test_every_type_and_phase_has_a_name);
  RUN_TEST(test_only_the_station_uplink_is_not_host_facing);
  RUN_TEST(test_the_usb_link_takes_its_subnet_from_the_mac);
  RUN_TEST(test_the_ppp_link_takes_the_next_subnet_over_from_the_same_byte);
  RUN_TEST(test_a_ppp_baud_must_be_listed_and_no_faster_than_tried);
  RUN_TEST(test_an_address_is_one_number_octets_first);
  return UNITY_END();
}
