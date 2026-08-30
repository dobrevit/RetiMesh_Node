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
  // Every combination of the four capabilities, by field: a positional list
  // here changed meaning when Caps grew a member, and swept the wrong rows.
  Caps combos[16];
  for (int i = 0; i < 16; i++) {
    combos[i].forceDownloadBoot = (i & 1) != 0;
    combos[i].nativeUsb         = (i & 2) != 0;
    combos[i].otgStack          = (i & 4) != 0;
    combos[i].bridgeAutoReset   = (i & 8) != 0;
  }
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
  TEST_ASSERT_EQUAL((int)Target::Bootloader, (int)s.snapshot().target);
  TEST_ASSERT_EQUAL((int)Source::Http, (int)s.snapshot().source);
}

static void test_the_composite_device_offers_software_entry_again() {
  // The serial-JTAG unit alone hangs the chip on a software entry; with the
  // OTG stack in charge the core hands the unit back before restarting.
  Caps jtag;      jtag.forceDownloadBoot = true; jtag.nativeUsb = true;
  Caps composite = jtag; composite.otgStack = true;
  TEST_ASSERT_FALSE(canEnterAutomatically(jtag));
  TEST_ASSERT_TRUE(canEnterAutomatically(composite));
  TEST_ASSERT_TRUE(plan(composite).has(Method::SoftwareApi));
  TEST_ASSERT_EQUAL((int)Method::SoftwareApi, (int)plan(composite).primary());
  TEST_ASSERT_EQUAL_STRING("", whyNotAutomatic(composite));
  TEST_ASSERT_TRUE(strlen(whyNotAutomatic(jtag)) > 0);
}

static void test_a_bootloader_request_outranks_a_pending_reboot() {
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(s.request(Target::Bootloader, Source::Console, 300, 100));
  TEST_ASSERT_EQUAL((int)Target::Bootloader, (int)s.snapshot().target);
  TEST_ASSERT_EQUAL_UINT32(400, s.snapshot().dueMs);
}

static void test_a_reboot_cannot_downgrade_a_pending_bootloader_entry() {
  // A flashing tool is waiting for the downloader; a settings save that
  // happens to land in the same second must not turn that into a plain
  // restart the tool will never see through.
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::Bootloader, Source::Http, 300, 0));
  TEST_ASSERT_FALSE(s.request(Target::App, Source::Settings, 1500, 10));
  TEST_ASSERT_EQUAL((int)Target::Bootloader, (int)s.snapshot().target);
  TEST_ASSERT_EQUAL_UINT32(300, s.snapshot().dueMs);
}

static void test_a_second_request_keeps_the_earlier_deadline() {
  // Two callers were each promised a delay. Honouring the later one would
  // push the restart past what the first was told — and a page that
  // re-posts on retry would keep a reboot from ever firing.
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Http, 1500, 1000));
  TEST_ASSERT_EQUAL_UINT32(1500, s.snapshot().dueMs);
  TEST_ASSERT_EQUAL((int)Source::Http, (int)s.snapshot().source);     // the latest asker is still on record
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(1500));
  // ...and an upgrade to the bootloader with a *shorter* delay moves it in.
  Sequencer u;
  TEST_ASSERT_TRUE(u.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(u.request(Target::Bootloader, Source::Console, 300, 100));
  TEST_ASSERT_EQUAL_UINT32(400, u.snapshot().dueMs);
  // A third request, later and longer, does not push the deadline out to
  // its own delay — but it does hold it off far enough for its own
  // acknowledgement to leave, which is the floor and nothing more.
  TEST_ASSERT_TRUE(u.request(Target::Bootloader, Source::Http, 600, 200));
  TEST_ASSERT_EQUAL_UINT32(200 + Sequencer::kAckFloorMs, u.snapshot().dueMs);
}

static void test_the_snapshot_says_how_long_until_the_restart() {
  // What STATUS and /api/status print: nothing while idle, the remaining
  // time while armed, and 0 rather than a wrapped number once it is due.
  Sequencer s;
  TEST_ASSERT_FALSE(s.snapshot().armed());
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().dueInMs(5));
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Http, 1500, 0));
  const Pending p = s.snapshot();
  TEST_ASSERT_TRUE(p.armed());
  TEST_ASSERT_EQUAL((int)State::Armed, (int)p.state);
  TEST_ASSERT_EQUAL_UINT32(500, p.dueInMs(1000));
  TEST_ASSERT_EQUAL_UINT32(0, p.dueInMs(1500));
  TEST_ASSERT_EQUAL_UINT32(0, p.dueInMs(1600));
}

static void test_a_request_is_never_answered_after_its_own_restart() {
  // A plain reboot armed long ago is about to fire; a bootloader request
  // lands a few milliseconds before it. Keeping the earlier deadline as it
  // stood would have restarted the node under its own 202. The deadline is
  // held off by at least the acknowledgement floor.
  Sequencer s;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Settings, 1500, 0));
  TEST_ASSERT_TRUE(s.request(Target::Bootloader, Source::Http, 600, 1495));
  TEST_ASSERT_EQUAL_UINT32(1495 + Sequencer::kAckFloorMs, s.snapshot().dueMs);
  TEST_ASSERT_EQUAL((int)Step::None, (int)s.tick(1500));
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(1495 + Sequencer::kAckFloorMs));
}

static void test_every_refusal_has_a_status() {
  TEST_ASSERT_EQUAL(202, httpStatus(Refusal::None));
  TEST_ASSERT_EQUAL(501, httpStatus(Refusal::CannotEnter));
  TEST_ASSERT_EQUAL(500, httpStatus(Refusal::CannotArm));
  TEST_ASSERT_EQUAL(409, httpStatus(Refusal::Busy));
  // ...and the reason a board cannot enter names the cause the plan used.
  Caps classic; classic.bridgeAutoReset = true;
  TEST_ASSERT_NOT_NULL(strstr(whyNotAutomatic(classic), "cannot enter"));
  Caps s3usb; s3usb.forceDownloadBoot = true; s3usb.nativeUsb = true;
  TEST_ASSERT_NOT_NULL(strstr(whyNotAutomatic(s3usb), "native-USB"));
  Caps bridged; bridged.forceDownloadBoot = true; bridged.bridgeAutoReset = true;
  TEST_ASSERT_EQUAL_STRING("", whyNotAutomatic(bridged));
}

static void test_nothing_is_accepted_once_quiescing() {
  Sequencer s;
  s.request(Target::App, Source::Http, 0, 0);
  // Even a zero delay waits out the acknowledgement floor.
  TEST_ASSERT_EQUAL((int)Step::None, (int)s.tick(0));
  const uint32_t t = Sequencer::kAckFloorMs;
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(t));
  TEST_ASSERT_FALSE(s.request(Target::Bootloader, Source::Console, 0, t + 1));
  TEST_ASSERT_EQUAL((int)Step::Restart, (int)s.tick(t + 1));
  TEST_ASSERT_FALSE(s.request(Target::App, Source::Console, 0, t + 2));
}

static void test_the_deadline_survives_millis_wraparound() {
  Sequencer s;
  const uint32_t nearWrap = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(s.request(Target::App, Source::Http, 0x200, nearWrap));
  TEST_ASSERT_EQUAL((int)Step::None, (int)s.tick(0xFFFFFFF0u));
  TEST_ASSERT_EQUAL((int)Step::Quiesce, (int)s.tick(0x00000100u));
}

// --- who may ask for the ROM downloader from off the cable -------------------
static void test_the_api_switch_refuses_every_remote_caller() {
  const char* why = nullptr;
  TEST_ASSERT_FALSE(remoteEntryAllowed(true, false, true, &why));    // host-facing, still refused
  TEST_ASSERT_NOT_NULL(strstr(why, "switched off"));
  TEST_ASSERT_FALSE(remoteEntryAllowed(false, false, false, &why));
}

static void test_a_host_facing_link_may_ask_when_the_api_is_on() {
  // The access point, usb0, ppp0: a caller there is as close to the node as
  // somebody holding it.
  const char* why = nullptr;
  TEST_ASSERT_TRUE(remoteEntryAllowed(true, true, false, &why));
  TEST_ASSERT_EQUAL_STRING("", why);
}

static void test_the_station_network_needs_its_own_permission() {
  // The default. Anyone on the home LAN with the admin password could
  // otherwise drop a relay into its ROM and leave it there.
  const char* why = nullptr;
  TEST_ASSERT_FALSE(remoteEntryAllowed(false, true, false, &why));
  TEST_ASSERT_NOT_NULL(strstr(why, "bootloader_from_lan"));
  TEST_ASSERT_TRUE(remoteEntryAllowed(false, true, true, &why));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_api_switch_refuses_every_remote_caller);
  RUN_TEST(test_a_host_facing_link_may_ask_when_the_api_is_on);
  RUN_TEST(test_the_station_network_needs_its_own_permission);
  RUN_TEST(test_a_classic_esp32_behind_a_bridge_offers_only_the_bridge);
  RUN_TEST(test_an_s3_behind_a_bridge_offers_the_api_and_the_bridge);
  RUN_TEST(test_native_usb_serial_jtag_offers_only_the_hardware_handshake);
  RUN_TEST(test_manual_recovery_is_always_last_and_always_there);
  RUN_TEST(test_every_method_has_a_name_and_the_list_joins_them);
  RUN_TEST(test_a_request_waits_out_its_delay_then_quiesces_then_restarts);
  RUN_TEST(test_the_composite_device_offers_software_entry_again);
  RUN_TEST(test_a_bootloader_request_outranks_a_pending_reboot);
  RUN_TEST(test_a_reboot_cannot_downgrade_a_pending_bootloader_entry);
  RUN_TEST(test_a_second_request_keeps_the_earlier_deadline);
  RUN_TEST(test_the_snapshot_says_how_long_until_the_restart);
  RUN_TEST(test_a_request_is_never_answered_after_its_own_restart);
  RUN_TEST(test_every_refusal_has_a_status);
  RUN_TEST(test_nothing_is_accepted_once_quiescing);
  RUN_TEST(test_the_deadline_survives_millis_wraparound);
  return UNITY_END();
}
