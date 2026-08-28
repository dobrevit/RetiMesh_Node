// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The composite USB device's endpoint budget. The ESP32-S3 has four usable
// IN endpoints and the planned device uses all four, so the margin is zero
// and the arithmetic deserves a test that says so in words.

#include <unity.h>
#include "UsbDescriptorPlan.h"

using namespace UsbPlan;

static void test_the_composite_fits_the_s3_with_no_in_endpoint_to_spare() {
  const Usage u = usage(COMPOSITE, COMPOSITE_COUNT);
  TEST_ASSERT_EQUAL_UINT8(4, u.inEndpoints);
  TEST_ASSERT_EQUAL_UINT8(2, u.outEndpoints);
  TEST_ASSERT_EQUAL_UINT8(4, u.interfaces);
  TEST_ASSERT_TRUE(fits(COMPOSITE, COMPOSITE_COUNT, ESP32S3));
  TEST_ASSERT_EQUAL_UINT8(ESP32S3.inEndpoints, u.inEndpoints);
}

static void test_a_third_cdc_function_would_not_enumerate() {
  // The obvious next request — a second ACM port for logs — is exactly the
  // one the silicon refuses. This is the check that turns that into a
  // compile error rather than a device that enumerates with a missing
  // interface.
  const Function three[] = { CDC_NCM, CDC_ACM, CDC_ACM };
  TEST_ASSERT_FALSE(fits(three, 3, ESP32S3));
}

static void test_a_function_with_no_in_endpoint_still_fits() {
  // Room remains on the OUT side and for interfaces; a vendor OUT-only
  // function could join if one were ever wanted.
  const Function four[] = { CDC_NCM, CDC_ACM, { "out-only", 0, 1, 1 } };
  TEST_ASSERT_TRUE(fits(four, 3, ESP32S3));
}

static void test_the_budget_matches_the_driver_constants() {
  // dcd_esp32sx.c: EP_FIFO_NUM 5 with FIFO0 reserved for EP0 -> 4 IN;
  // EP_MAX 7 with EP0 reserved -> 6 OUT. If someone edits the budget, this
  // is where they find out what it was derived from.
  TEST_ASSERT_EQUAL_UINT8(5 - 1, ESP32S3.inEndpoints);
  TEST_ASSERT_EQUAL_UINT8(7 - 1, ESP32S3.outEndpoints);
}

static void test_identity_is_flagged_as_provisional() {
  // The strings are ours; the PID is not, yet. Anything that packages a
  // release should be able to read this and refuse.
  TEST_ASSERT_EQUAL_STRING("RetiMesh", MANUFACTURER);
  TEST_ASSERT_EQUAL_STRING("RetiMesh Node", PRODUCT);
  TEST_ASSERT_TRUE(PID_IS_TEST_ALLOCATION);
  TEST_ASSERT_EQUAL_HEX16(0x1209, VID);
  TEST_ASSERT_EQUAL_HEX16(0x303A, ESPRESSIF_VID);
  TEST_ASSERT_EQUAL_HEX16(0x1001, ESP_USB_SERIAL_JTAG_PID);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_composite_fits_the_s3_with_no_in_endpoint_to_spare);
  RUN_TEST(test_a_third_cdc_function_would_not_enumerate);
  RUN_TEST(test_a_function_with_no_in_endpoint_still_fits);
  RUN_TEST(test_the_budget_matches_the_driver_constants);
  RUN_TEST(test_identity_is_flagged_as_provisional);
  return UNITY_END();
}
