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

static void test_an_address_is_one_number_octets_first() {
  // hostOrder() and the trust rule compare these; the first octet has to be
  // the most significant or two addresses on one link compare unequal.
  TEST_ASSERT_EQUAL_UINT32(0x0A2A0001u, ipv4(10, 42, 0, 1));
  TEST_ASSERT_NOT_EQUAL(ipv4(10, 42, 0, 1), ipv4(1, 0, 42, 10));
}

int main() {
  UNITY_BEGIN();
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
  RUN_TEST(test_an_address_is_one_number_octets_first);
  return UNITY_END();
}
