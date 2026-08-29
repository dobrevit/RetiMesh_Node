// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The bootloader plan and the request sequence. Both decide whether a node
// reboots, and one of them decides whether it reboots into a mode where it
// runs no firmware at all — so the cases here are mostly the ones where the
// answer must be "no": a reboot asked for while a flash is already pending,
// a board whose silicon cannot do it, a second request that would have
// pushed a promised deadline out.

#include <unity.h>
#include <string.h>
#include <initializer_list>
#include "BootloaderPlan.h"

using namespace Bootloader;

// --- plan(): what a board offers --------------------------------------------

static void test_a_classic_esp32_behind_a_bridge_offers_only_the_bridge() {
  // T-Beam (CH9102), Wireless Stick (CP2102): no FORCE_DOWNLOAD_BOOT bit, so
  // the firmware cannot get itself into the downloader. esptool can.
  Caps c; c.bridgeAutoReset = true;
  const Plan p = plan(c);
  TEST_ASSERT_FALSE(canEnterAutomatically(c));
  TEST_ASSERT_FALSE(p.has(Method::SoftwareApi));
  TEST_ASSERT_TRUE(p.has(Method::AutoResetDtrRts));
  TEST_ASSERT_EQUAL((int)Method::AutoResetDtrRts, (int)p.primary());
}

static void test_an_s3_behind_a_bridge_offers_the_api_and_the_bridge() {
  // Heltec V3: S3 silicon, CP2102 on UART0. The ROM downloader listens on
  // UART0, so a software transition and esptool's own reset both work.
  Caps c; c.forceDownloadBoot = true; c.bridgeAutoReset = true;
  const Plan p = plan(c);
  TEST_ASSERT_TRUE(canEnterAutomatically(c));
  TEST_ASSERT_EQUAL((int)Method::SoftwareApi, (int)p.primary());
  TEST_ASSERT_TRUE(p.has(Method::AutoResetDtrRts));
}

static void test_native_usb_serial_jtag_offers_only_the_hardware_handshake() {
  // T3-S3: S3 silicon, so the bit exists — but the console is the chip's own
  // USB-Serial/JTAG unit, which survives a software reset, and a downloader
  // entered that way hung the node until EN was pulled. The unit does the
  // DTR/RTS handshake in hardware; that is the method, and the only one.
  Caps c; c.forceDownloadBoot = true; c.nativeUsb = true;
  const Plan p = plan(c);
  TEST_ASSERT_FALSE(canEnterAutomatically(c));
  TEST_ASSERT_FALSE(p.has(Method::SoftwareApi));
  TEST_ASSERT_EQUAL((int)Method::AutoResetDtrRts, (int)p.primary());
}

static void test_manual_recovery_is_always_last_and_always_there() {
  const Caps combos[] = { Caps{}, Caps{true, false, false}, Caps{true, true, false},
                          Caps{false, false, true}, Caps{true, true, true} };
  for (const Caps& c : combos) {
    const Plan p = plan(c);
    TEST_ASSERT_TRUE(p.count >= 1);
    TEST_ASSERT_EQUAL((int)Method::ManualRecovery, (int)p.methods[p.count - 1]);
  }
}

static void test_every_method_has_a_name_and_the_list_joins_them() {
  const Method all[] = { Method::SoftwareApi, Method::AutoResetDtrRts, Method::ManualRecovery };
  for (Method m : all) TEST_ASSERT_NOT_EQUAL(0, strcmp(methodName(m), "unknown"));
  Caps c; c.forceDownloadBoot = true; c.bridgeAutoReset = true;   // an S3 behind a bridge
  char buf[64];
  TEST_ASSERT_EQUAL_STRING("software_api,auto_reset_dtr_rts,manual_recovery", plan(c).names(buf, sizeof(buf)));
  // A buffer too small for the list is cut, not overrun, and still terminated.
  char tiny[8];
  plan(c).names(tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL('\0', tiny[sizeof(tiny) - 1]);
}

// --- Sequencer ---------------------------------------------------------------

static void test_a_request_waits_out_its_delay_then_quiesces_then_restarts() {
  Sequencer s;
  TEST_ASSERT_FALSE(s.pending());
  TEST_ASSERT_TRUE(s.request(Target::Bootloader, Source::Http, 500, 1000));
  TEST_ASSERT_TRUE(s.pending());
  TEST_ASSERT_EQUAL((int)Step::None, (int)s.tick(1000));
  TEST_ASSERT_EQUAL((int)Step::None, (int)s.tick(1499));
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(1500));
  TEST_ASSERT_EQUAL((int)State::Quiescing, (int)s.state());
  TEST_ASSERT_EQUAL((int)Step::Restart, (int)s.tick(1501));
  TEST_ASSERT_EQUAL((int)State::Restarting, (int)s.state());
  // If the restart somehow returned, the next tick asks again rather than
  // pretending the node is idle.
  TEST_ASSERT_EQUAL((int)Step::Restart, (int)s.tick(1600));
  TEST_ASSERT_EQUAL((int)Target::Bootloader, (int)s.target());
  TEST_ASSERT_EQUAL((int)Source::Http, (int)s.source());
}

static void test_a_bootloader_request_outranks_a_pending_reboot() {
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(s.request(Target::Bootloader, Source::Console, 300, 100));
  TEST_ASSERT_EQUAL((int)Target::Bootloader, (int)s.target());
  TEST_ASSERT_EQUAL_UINT32(400, s.dueMs());
}

static void test_a_reboot_cannot_downgrade_a_pending_bootloader_entry() {
  // A flashing tool is waiting for the downloader; a settings save that
  // happens to land in the same second must not turn that into a plain
  // restart the tool will never see through.
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::Bootloader, Source::Http, 300, 0));
  TEST_ASSERT_FALSE(s.request(Target::App, Source::Settings, 1500, 10));
  TEST_ASSERT_EQUAL((int)Target::Bootloader, (int)s.target());
  TEST_ASSERT_EQUAL_UINT32(300, s.dueMs());
}

static void test_a_second_request_keeps_the_earlier_deadline() {
  // Two callers were each promised a delay. Honouring the later one would
  // push the restart past what the first was told — and a page that
  // re-posts on retry would keep a reboot from ever firing.
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Http, 1500, 1000));
  TEST_ASSERT_EQUAL_UINT32(1500, s.dueMs());
  TEST_ASSERT_EQUAL((int)Source::Http, (int)s.source());     // the latest asker is still on record
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(1500));
  // ...and an upgrade to the bootloader with a *shorter* delay moves it in.
  Sequencer u;
  TEST_ASSERT_TRUE(u.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(u.request(Target::Bootloader, Source::Console, 300, 100));
  TEST_ASSERT_EQUAL_UINT32(400, u.dueMs());
  TEST_ASSERT_TRUE(u.request(Target::Bootloader, Source::Http, 600, 200));   // later and longer: ignored
  TEST_ASSERT_EQUAL_UINT32(400, u.dueMs());
}

static void test_nothing_is_accepted_once_quiescing() {
  Sequencer s;
  s.request(Target::App, Source::Http, 0, 0);
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(0));
  TEST_ASSERT_FALSE(s.request(Target::Bootloader, Source::Console, 0, 1));
  TEST_ASSERT_EQUAL((int)Step::Restart, (int)s.tick(1));
  TEST_ASSERT_FALSE(s.request(Target::App, Source::Console, 0, 2));
}

static void test_the_deadline_survives_millis_wraparound() {
  Sequencer s;
  const uint32_t nearWrap = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Http, 0x200, nearWrap));
  TEST_ASSERT_EQUAL((int)Step::None, (int)s.tick(0xFFFFFFF0u));
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(0x00000100u));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_classic_esp32_behind_a_bridge_offers_only_the_bridge);
  RUN_TEST(test_an_s3_behind_a_bridge_offers_the_api_and_the_bridge);
  RUN_TEST(test_native_usb_serial_jtag_offers_only_the_hardware_handshake);
  RUN_TEST(test_manual_recovery_is_always_last_and_always_there);
  RUN_TEST(test_every_method_has_a_name_and_the_list_joins_them);
  RUN_TEST(test_a_request_waits_out_its_delay_then_quiesces_then_restarts);
  RUN_TEST(test_a_bootloader_request_outranks_a_pending_reboot);
  RUN_TEST(test_a_reboot_cannot_downgrade_a_pending_bootloader_entry);
  RUN_TEST(test_a_second_request_keeps_the_earlier_deadline);
  RUN_TEST(test_nothing_is_accepted_once_quiescing);
  RUN_TEST(test_the_deadline_survives_millis_wraparound);
  return UNITY_END();
}
