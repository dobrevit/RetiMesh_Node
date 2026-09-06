// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
//
// The per-chip interrupt-bit tables, against RadioLib's own headers.
// Runs on the host: pio test -e native
//
// LoRaRadio::rxDoneFlag() and LoRaRadio::cadDoneFlag() each hand-copy four
// vendors' IRQ expressions, because PhysicalLayer::getIrqFlags() looks generic
// and is not: every driver returns its raw hardware register and the bits do
// not line up between parts. One wrong bit is not a compile error and not a
// crash — it is a family whose receiver never fires, or whose every carrier
// sense probe runs to its deadline and reports a busy channel. Both have
// happened: rxDoneFlag() exists because a single generic constant tested
// against three parts was right for two of them by coincidence, and the 2.4 GHz
// boards kept working while both sub-GHz boards stopped receiving altogether.
//
// The driver itself cannot be compiled on the host, so this restates the two
// tables' expressions and pins them from both sides:
//
//   * against the literal masks the driver's own comments name, so a RadioLib
//     bump that renumbers a bit fails here rather than on a node; and
//   * for the three families that fill their map in the constructor, against
//     RadioLib's own per-chip irqMap, reached through getIrqMapped(). That is
//     an independent source of truth inside the library for which raw bit each
//     named event is, so choosing the wrong macro fails too.
//
// The SX127x is the exception, and the exception is the reason cadDoneFlag()
// keeps its explicit table rather than calling getIrqMapped(): SX127x fills
// irqMap inside begin(), so the library's answer is zero before the part has
// been started and beginFSK() later overwrites both CAD entries with
// RADIOLIB_IRQ_NOT_SUPPORTED. The explicit table is a pure function of the
// detected part; the library's is state.
#include <unity.h>
#include <RadioLib.h>

// Never touched: the constructors only store the pointer, and nothing below
// goes near the bus. begin() is what would need a HAL, and is never called.
static Module mod(nullptr, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC);
static SX1276 sx1276(&mod);
static SX1262 sx1262(&mod);
static SX1280 sx1280(&mod);
static LR1110 lr1110(&mod);

// The two expressions, exactly as the driver writes them.
static const uint32_t kCadSX127x = RADIOLIB_SX127X_CLEAR_IRQ_FLAG_CAD_DONE |
                                   RADIOLIB_SX127X_CLEAR_IRQ_FLAG_CAD_DETECTED;
static const uint32_t kCadSX126x = RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED;
static const uint32_t kCadSX128x = RADIOLIB_SX128X_IRQ_CAD_DONE | RADIOLIB_SX128X_IRQ_CAD_DETECTED;
static const uint32_t kCadLR11x0 = RADIOLIB_LR11X0_IRQ_CAD_DONE | RADIOLIB_LR11X0_IRQ_CAD_DETECTED;

static const uint32_t kRxSX127x = RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE;
static const uint32_t kRxSX126x = RADIOLIB_SX126X_IRQ_RX_DONE;
static const uint32_t kRxSX128x = RADIOLIB_SX128X_IRQ_RX_DONE;
static const uint32_t kRxLR11x0 = RADIOLIB_LR11X0_IRQ_RX_DONE;

// RadioLib's own names for the two events, in the generic vocabulary its map is
// indexed by.
static const RadioLibIrqFlags_t kIrqCad = (1UL << RADIOLIB_IRQ_CAD_DONE) |
                                          (1UL << RADIOLIB_IRQ_CAD_DETECTED);
static const RadioLibIrqFlags_t kIrqRx  = (1UL << RADIOLIB_IRQ_RX_DONE);

// The bit positions cadDoneFlag()'s comment names: CAD-done is bit 2 on an
// SX127x, bit 7 on an SX126x, bit 12 on an SX128x and bit 8 on an LR11x0, and
// each part raises "detected" in the bit next to it — which is why both are in
// the mask. A scan that set only one of a two-bit answer would be a probe this
// firmware kept waiting for after the chip had already given its verdict.
static void test_the_cad_masks_are_the_bits_the_driver_documents() {
  TEST_ASSERT_EQUAL_HEX32((1UL << 2) | (1UL << 0),  kCadSX127x);   // 0x0005
  TEST_ASSERT_EQUAL_HEX32((1UL << 7) | (1UL << 8),  kCadSX126x);   // 0x0180
  TEST_ASSERT_EQUAL_HEX32((1UL << 12) | (1UL << 13), kCadSX128x);  // 0x3000
  TEST_ASSERT_EQUAL_HEX32((1UL << 8) | (1UL << 9),  kCadLR11x0);   // 0x0300

  // Two bits each, never one: a mask that lost its "detected" half would still
  // compile and would still be right on whichever part happens to raise done
  // first.
  TEST_ASSERT_NOT_EQUAL_UINT32(0, kCadSX127x & ~(uint32_t)RADIOLIB_SX127X_CLEAR_IRQ_FLAG_CAD_DONE);
  TEST_ASSERT_NOT_EQUAL_UINT32(0, kCadSX126x & ~(uint32_t)RADIOLIB_SX126X_IRQ_CAD_DONE);
  TEST_ASSERT_NOT_EQUAL_UINT32(0, kCadSX128x & ~(uint32_t)RADIOLIB_SX128X_IRQ_CAD_DONE);
  TEST_ASSERT_NOT_EQUAL_UINT32(0, kCadLR11x0 & ~(uint32_t)RADIOLIB_LR11X0_IRQ_CAD_DONE);
}

// The same for the other event, which carries the identical gap: rxDoneFlag()
// is bit 6 on an SX127x and bit 1 on an SX126x or SX128x, and the coincidence
// between the last two is exactly what hid the original bug.
static void test_the_rx_done_masks_are_the_bits_the_driver_documents() {
  TEST_ASSERT_EQUAL_HEX32(1UL << 6, kRxSX127x);                    // 0b01000000
  TEST_ASSERT_EQUAL_HEX32(1UL << 1, kRxSX126x);                    // 0b10
  TEST_ASSERT_EQUAL_HEX32(1UL << 1, kRxSX128x);                    // 0x0002
  TEST_ASSERT_EQUAL_HEX32(1UL << 3, kRxLR11x0);                    // 1 << 3
}

// Both tables are read off the same register in the same task — handleRadioIrq()
// tests the RxDone mask, mediumFree() the CAD mask — so on no part may one
// event's bits appear inside the other's, or a finished scan would be collected
// as a packet.
static void test_no_part_confuses_a_finished_scan_with_a_packet() {
  TEST_ASSERT_EQUAL_HEX32(0, kCadSX127x & kRxSX127x);
  TEST_ASSERT_EQUAL_HEX32(0, kCadSX126x & kRxSX126x);
  TEST_ASSERT_EQUAL_HEX32(0, kCadSX128x & kRxSX128x);
  TEST_ASSERT_EQUAL_HEX32(0, kCadLR11x0 & kRxLR11x0);
}

// The independent check: RadioLib's own map for each part, which is where the
// library itself looks up "which raw bit is CAD-done on this chip". Choosing
// the wrong macro name would pass every assertion above and fail here.
static void test_the_masks_agree_with_radiolibs_own_map() {
  TEST_ASSERT_EQUAL_HEX32(kCadSX126x, sx1262.getIrqMapped(kIrqCad));
  TEST_ASSERT_EQUAL_HEX32(kCadSX128x, sx1280.getIrqMapped(kIrqCad));
  TEST_ASSERT_EQUAL_HEX32(kCadLR11x0, lr1110.getIrqMapped(kIrqCad));

  TEST_ASSERT_EQUAL_HEX32(kRxSX126x, sx1262.getIrqMapped(kIrqRx));
  TEST_ASSERT_EQUAL_HEX32(kRxSX128x, sx1280.getIrqMapped(kIrqRx));
  TEST_ASSERT_EQUAL_HEX32(kRxLR11x0, lr1110.getIrqMapped(kIrqRx));
}

// ...and why the SX127x is not in the list above, which is the whole argument
// for keeping the explicit table. Its map is filled in begin(), so an
// unstarted part answers zero for both events — an answer that would read as
// "this chip has no CAD bits" and, before the fix in mediumFree(), as a scan
// that had completed. Pinned so a RadioLib version that moved the fill into the
// constructor is noticed as a change rather than assumed.
static void test_the_sx127x_map_is_state_not_a_table() {
  TEST_ASSERT_EQUAL_HEX32(0, sx1276.getIrqMapped(kIrqCad));
  TEST_ASSERT_EQUAL_HEX32(0, sx1276.getIrqMapped(kIrqRx));
  TEST_ASSERT_NOT_EQUAL_UINT32(0, kCadSX127x);
  TEST_ASSERT_NOT_EQUAL_UINT32(0, kRxSX127x);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_cad_masks_are_the_bits_the_driver_documents);
  RUN_TEST(test_the_rx_done_masks_are_the_bits_the_driver_documents);
  RUN_TEST(test_no_part_confuses_a_finished_scan_with_a_packet);
  RUN_TEST(test_the_masks_agree_with_radiolibs_own_map);
  RUN_TEST(test_the_sx127x_map_is_state_not_a_table);
  return UNITY_END();
}
