// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The bootloader plan, the request sequence and the 1200-baud touch. All
// three decide whether a node reboots, and one of them decides whether it
// reboots into a mode where it runs no firmware at all — so the cases here
// are mostly the ones where the answer must be "no": a console opened at
// the wrong rate, a reboot asked for while a flash is already pending, a
// board whose silicon cannot do it.

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
  TEST_ASSERT_FALSE(p.has(Method::Usb1200Touch));
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
  TEST_ASSERT_FALSE(p.has(Method::Usb1200Touch));
}

static void test_native_usb_serial_jtag_counts_as_auto_reset() {
  // T3-S3 today: the fixed USB-Serial/JTAG peripheral implements the DTR/RTS
  // handshake in hardware, and the firmware never sees a line coding — so no
  // touch, but the bridge-style reset applies.
  Caps c; c.forceDownloadBoot = true; c.nativeUsb = true;
  const Plan p = plan(c);
  TEST_ASSERT_TRUE(p.has(Method::AutoResetDtrRts));
  TEST_ASSERT_FALSE(p.has(Method::Usb1200Touch));
  TEST_ASSERT_EQUAL((int)Method::SoftwareApi, (int)p.primary());
}

static void test_native_usb_with_an_otg_cdc_prefers_the_touch() {
  // T3-S3 once the composite device runs: the firmware owns the CDC port,
  // sees the 1200-baud line coding, and the hardware handshake is gone.
  Caps c; c.forceDownloadBoot = true; c.nativeUsb = true; c.usbCdcOtg = true;
  const Plan p = plan(c);
  TEST_ASSERT_EQUAL((int)Method::Usb1200Touch, (int)p.primary());
  TEST_ASSERT_TRUE(p.has(Method::SoftwareApi));
  TEST_ASSERT_FALSE(p.has(Method::AutoResetDtrRts));
}

static void test_manual_recovery_is_always_last_and_always_there() {
  const Caps combos[] = { Caps{}, Caps{true, false, false, false}, Caps{true, true, true, false},
                          Caps{false, false, false, true}, Caps{true, true, true, true} };
  for (const Caps& c : combos) {
    const Plan p = plan(c);
    TEST_ASSERT_TRUE(p.count >= 1);
    TEST_ASSERT_EQUAL((int)Method::ManualRecovery, (int)p.methods[p.count - 1]);
  }
  // A touch needs a CDC the firmware owns *and* that CDC on the connector.
  Caps odd; odd.usbCdcOtg = true; odd.nativeUsb = false;
  TEST_ASSERT_FALSE(plan(odd).has(Method::Usb1200Touch));
}

static void test_every_method_has_a_name() {
  const Method all[] = { Method::Usb1200Touch, Method::SoftwareApi, Method::AutoResetDtrRts, Method::ManualRecovery };
  for (Method m : all) TEST_ASSERT_NOT_EQUAL(0, strcmp(methodName(m), "unknown"));
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

// --- TouchDetector -----------------------------------------------------------

static void test_the_touch_is_1200_then_open_then_close() {
  TouchDetector t;
  t.onLineCoding(1200, 0);
  TEST_ASSERT_FALSE(t.onLineState(true, true, 10));     // opened
  TEST_ASSERT_TRUE(t.onLineState(false, false, 20));    // closed: fire
  // One touch fires once.
  TEST_ASSERT_FALSE(t.onLineState(false, false, 30));
  TEST_ASSERT_FALSE(t.armed());
}

static void test_a_console_at_any_other_rate_never_fires() {
  TouchDetector t;
  for (uint32_t baud : { 9600u, 115200u, 921600u, 1201u, 12000u }) {
    t.onLineCoding(baud, 0);
    TEST_ASSERT_FALSE(t.onLineState(true, true, 10));
    TEST_ASSERT_FALSE(t.onLineState(false, false, 20));
  }
}

static void test_changing_the_rate_before_closing_disarms() {
  // A terminal opened at 1200 by mistake and switched to 115200 before it is
  // closed is a person, not a flashing tool.
  TouchDetector t;
  t.onLineCoding(1200, 0);
  t.onLineState(true, true, 10);
  t.onLineCoding(115200, 20);
  TEST_ASSERT_FALSE(t.onLineState(false, false, 30));
}

static void test_a_close_without_an_open_does_not_fire() {
  // 1200 set with DTR already low: no port was opened, so none was closed.
  // Some host drivers send the line state before the line coding, so the
  // detector stays armed for the open-then-close that may still follow.
  TouchDetector t;
  t.onLineCoding(1200, 0);
  TEST_ASSERT_FALSE(t.onLineState(false, false, 10));
  TEST_ASSERT_TRUE(t.armed());
  TEST_ASSERT_FALSE(t.onLineState(true, true, 20));
  TEST_ASSERT_TRUE(t.onLineState(false, false, 30));
}

static void test_the_window_expires() {
  TouchDetector t(1200, 5000);
  t.onLineCoding(1200, 0);
  t.onLineState(true, true, 10);
  TEST_ASSERT_FALSE(t.onLineState(false, false, 6000));  // too late
}

static void test_payload_bytes_are_never_consulted() {
  // There is no API for data at all; the detector only sees line coding and
  // line state. This test exists to keep it that way.
  TouchDetector t;
  TEST_ASSERT_FALSE(t.onLineState(true, true, 0));
  TEST_ASSERT_FALSE(t.onLineState(false, false, 1));
}

static void test_the_magic_baud_is_configurable() {
  TouchDetector t(2400);
  t.onLineCoding(1200, 0);
  t.onLineState(true, true, 1);
  TEST_ASSERT_FALSE(t.onLineState(false, false, 2));
  t.onLineCoding(2400, 3);
  t.onLineState(true, true, 4);
  TEST_ASSERT_TRUE(t.onLineState(false, false, 5));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_classic_esp32_behind_a_bridge_offers_only_the_bridge);
  RUN_TEST(test_an_s3_behind_a_bridge_offers_the_api_and_the_bridge);
  RUN_TEST(test_native_usb_serial_jtag_counts_as_auto_reset);
  RUN_TEST(test_native_usb_with_an_otg_cdc_prefers_the_touch);
  RUN_TEST(test_manual_recovery_is_always_last_and_always_there);
  RUN_TEST(test_every_method_has_a_name);
  RUN_TEST(test_a_request_waits_out_its_delay_then_quiesces_then_restarts);
  RUN_TEST(test_a_bootloader_request_outranks_a_pending_reboot);
  RUN_TEST(test_a_reboot_cannot_downgrade_a_pending_bootloader_entry);
  RUN_TEST(test_nothing_is_accepted_once_quiescing);
  RUN_TEST(test_the_deadline_survives_millis_wraparound);
  RUN_TEST(test_the_touch_is_1200_then_open_then_close);
  RUN_TEST(test_a_console_at_any_other_rate_never_fires);
  RUN_TEST(test_changing_the_rate_before_closing_disarms);
  RUN_TEST(test_a_close_without_an_open_does_not_fire);
  RUN_TEST(test_the_window_expires);
  RUN_TEST(test_payload_bytes_are_never_consulted);
  RUN_TEST(test_the_magic_baud_is_configurable);
  return UNITY_END();
}
