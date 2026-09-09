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


// The BME280's readings are a polynomial over eleven factory coefficients, and
// this pins the parts of that which have an authority outside the source file:
// how the coefficients are decoded out of the register blocks, how a converter
// count is assembled out of three registers, the clamps, and the one division
// the pressure polynomial has to guard. Those rules come off the part's
// datasheet, so a test can be wrong about them and be told so.
//
// What is deliberately *not* here is any assertion that a compensated number
// is the true temperature of the room. Nothing on a host can know that, and a
// test that computed the expectation with the same polynomial would be the
// polynomial agreeing with itself — a green light for a transcription error in
// both places. The absolute readings are a bench item: a plausible room
// temperature, and a pressure that agrees with another instrument.
//
// That was the whole of this suite until a review demonstrated what it let
// through: a divisor out by a factor of two — 2^33 written as 2^32 in the
// pressure path, or /2048 as /1024 in t_fine — halves or doubles every reading
// and passes all of it, because a clamp accepts 50 hPa and monotonicity does
// not care about slope. Two things close that, and both are below.
//
// The coefficient set turns out to be the datasheet's own worked example,
// published so that an implementation can check itself, so the readings are
// pinned absolutely against it. And the polynomial is written a second time in
// the datasheet's floating-point form — different constants, different
// operations — with the two required to agree.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/Bme280Math.h"

// A coefficient set of the shape a real part reports: t1 large and unsigned,
// t2 positive, t3 small and negative, the pressure set spanning both signs.
static Bme280::Calibration plausible() {
  Bme280::Calibration c;
  c.t1 = 27504; c.t2 = 26435; c.t3 = -1000;
  c.p1 = 36477; c.p2 = -10685; c.p3 = 3024; c.p4 = 2855; c.p5 = 140;
  c.p6 = -7; c.p7 = 15500; c.p8 = -14600; c.p9 = 6000;
  c.h1 = 75; c.h2 = 362; c.h3 = 0; c.h4 = 302; c.h5 = 50; c.h6 = 30;
  return c;
}

// ---------------------------------------------------------------------------
// Assembling a converter count out of its registers

// Twenty bits from three registers: eight, eight, and the top four of the
// third. The low four bits of the xlsb are not data and must not arrive as
// data — a driver that shifts by the wrong amount there reads a value that
// moves in the right direction and is wrong by a factor.
static void test_a_twenty_bit_count_is_msb_lsb_and_the_top_nibble() {
  const uint8_t d[8] = { 0x51, 0x23, 0xF0,   // pressure  0x51230 + 0xf>>... = 0x5123F
                         0x82, 0x34, 0x50,   // temperature
                         0x00, 0x00 };
  const Bme280::Raw r = Bme280::decodeRaw(d);
  TEST_ASSERT_EQUAL_UINT32(0x5123FU, r.pressure);
  TEST_ASSERT_EQUAL_UINT32(0x82345U, r.temperature);
}

// And the low nibble of the xlsb is discarded rather than added in.
static void test_the_unused_nibble_of_the_xlsb_is_not_data() {
  const uint8_t with[8]    = { 0x51, 0x23, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00 };
  const uint8_t without[8] = { 0x51, 0x23, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00 };
  TEST_ASSERT_EQUAL_UINT32(Bme280::decodeRaw(without).pressure,
                           Bme280::decodeRaw(with).pressure);
}

static void test_humidity_is_sixteen_bits_of_two_registers() {
  const uint8_t d[8] = { 0, 0, 0, 0, 0, 0, 0x7F, 0xE3 };
  TEST_ASSERT_EQUAL_UINT32(0x7FE3U, Bme280::decodeRaw(d).humidity);
}

// The full-scale count, which is what a saturated converter reports and what
// the pressure polynomial has to survive without overflowing.
static void test_full_scale_counts_decode_to_their_limits() {
  const uint8_t d[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  const Bme280::Raw r = Bme280::decodeRaw(d);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFU, r.pressure);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFU, r.temperature);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFU,  r.humidity);
}

// ---------------------------------------------------------------------------
// Decoding the calibration

// Little endian, and six of the eleven signed. A coefficient read unsigned
// where it should be signed gives a reading that is wrong at one end of the
// range and plausible at the other, which is the hardest kind of wrong to
// notice on a bench.
static void test_calibration_is_little_endian_and_the_signed_ones_are_signed() {
  uint8_t a[26] = {0};
  uint8_t b[7]  = {0};
  a[0] = 0x70; a[1] = 0x6B;                    // t1 = 0x6B70 unsigned
  a[2] = 0x43; a[3] = 0x67;                    // t2 = 0x6743 positive
  a[4] = 0x18; a[5] = 0xFC;                    // t3 = 0xFC18 = -1000 signed
  a[8] = 0x53; a[9] = 0xD6;                    // p2 = 0xD653 negative
  a[25] = 0x4B;                                // h1
  const Bme280::Calibration c = Bme280::decodeCalibration(a, b);
  TEST_ASSERT_EQUAL_UINT16(0x6B70, c.t1);
  TEST_ASSERT_EQUAL_INT16(0x6743, c.t2);
  TEST_ASSERT_EQUAL_INT16(-1000, c.t3);
  TEST_ASSERT_EQUAL_INT16((int16_t)0xD653, c.p2);
  TEST_ASSERT_TRUE(c.p2 < 0);
  TEST_ASSERT_EQUAL_UINT8(0x4B, c.h1);
}

// h4 and h5 share the byte at 0xe5, and which nibble belongs to which is the
// classic error with this part: h4 takes the LOW nibble, h5 the HIGH one. Both
// coefficients come out plausible either way round, and the humidity is then
// merely wrong.
static void test_h4_and_h5_split_the_shared_byte_low_nibble_to_h4() {
  uint8_t a[26] = {0};
  uint8_t b[7]  = {0};
  b[3] = 0x01;                                 // h4 high bits
  b[4] = 0xA5;                                 // shared: low nibble 5, high nibble A
  b[5] = 0x02;                                 // h5 high bits
  const Bme280::Calibration c = Bme280::decodeCalibration(a, b);
  TEST_ASSERT_EQUAL_INT16(1 * 16 + 0x5, c.h4);
  TEST_ASSERT_EQUAL_INT16(2 * 16 + 0xA, c.h5);
  // and swapping the nibbles has to change the answer, or this test would
  // pass against a driver that read them the wrong way round
  TEST_ASSERT_NOT_EQUAL(c.h4, c.h5);
}

// Both are signed through their high byte, so a negative one has to survive
// the shift rather than becoming a large positive.
static void test_a_negative_h4_stays_negative_through_the_shift() {
  uint8_t a[26] = {0};
  uint8_t b[7]  = {0};
  b[3] = 0xFF;                                 // -1 in the high bits
  b[4] = 0x00;
  const Bme280::Calibration c = Bme280::decodeCalibration(a, b);
  TEST_ASSERT_TRUE(c.h4 < 0);
  TEST_ASSERT_EQUAL_INT16(-16, c.h4);
}

static void test_h6_is_signed() {
  uint8_t a[26] = {0};
  uint8_t b[7]  = {0};
  b[6] = 0xE2;                                 // -30
  TEST_ASSERT_EQUAL_INT8(-30, Bme280::decodeCalibration(a, b).h6);
}

// ---------------------------------------------------------------------------
// The clamps, which are the datasheet's and are not cosmetic

static void test_temperature_is_clamped_to_the_specified_range() {
  TEST_ASSERT_EQUAL_INT32(-4000, Bme280::temperatureCentiC(-2000000));
  TEST_ASSERT_EQUAL_INT32(8500,  Bme280::temperatureCentiC(2000000));
  // and something in range passes through untouched
  const int32_t mid = Bme280::temperatureCentiC(128422);
  TEST_ASSERT_TRUE(mid > -4000 && mid < 8500);
}

static void test_pressure_is_clamped_to_the_specified_range() {
  const Bme280::Calibration c = plausible();
  // The whole twenty-bit domain, at a plausible temperature: every answer has
  // to land inside the specified range, which is also the check that the
  // 64-bit intermediates never overflow into nonsense.
  for (uint32_t adc = 0; adc <= 0xFFFFF; adc += 0x400) {
    const uint32_t p = Bme280::pressureCentiPa(c, 128422, adc);
    TEST_ASSERT_TRUE(p >= 3000000U && p <= 11000000U);
  }
}

static void test_humidity_never_exceeds_one_hundred_percent() {
  const Bme280::Calibration c = plausible();
  for (uint32_t adc = 0; adc <= 0xFFFF; adc += 0x40) {
    const uint32_t h = Bme280::humidityQ1024(c, 128422, adc);
    TEST_ASSERT_TRUE(h <= 102400U);
  }
}

// ---------------------------------------------------------------------------
// The division the pressure polynomial has to guard

// p1 is a divisor. A part that never answered, or a calibration block that
// arrived as zeroes, would divide by nothing — so the floor is returned
// instead, which is the polynomial admitting it has no reading rather than
// faulting on the RNS task's pass.
static void test_a_zero_p1_returns_the_floor_instead_of_dividing_by_it() {
  Bme280::Calibration c = plausible();
  c.p1 = 0;
  TEST_ASSERT_EQUAL_UINT32(3000000U, Bme280::pressureCentiPa(c, 128422, 415148));
}

static void test_an_all_zero_calibration_does_not_fault() {
  const Bme280::Calibration zero;
  TEST_ASSERT_EQUAL_UINT32(3000000U, Bme280::pressureCentiPa(zero, 0, 0));
  const uint32_t h = Bme280::humidityQ1024(zero, 0, 0);
  TEST_ASSERT_TRUE(h <= 102400U);
  const int32_t t = Bme280::temperatureCentiC(Bme280::tFine(zero, 0));
  TEST_ASSERT_TRUE(t >= -4000 && t <= 8500);
}

// ---------------------------------------------------------------------------
// Structure: what the polynomial must do even where the absolute value is a
// bench question

// A hotter converter count has to read hotter. This holds for any calibration
// with a positive t2, which is every real part — and it is the property that
// catches a sign or a shift dropped somewhere in tFine().
static void test_a_higher_count_reads_hotter() {
  const Bme280::Calibration c = plausible();
  int32_t last = Bme280::temperatureCentiC(Bme280::tFine(c, 0));
  for (uint32_t adc = 0x1000; adc <= 0xFFFFF; adc += 0x1000) {
    const int32_t t = Bme280::temperatureCentiC(Bme280::tFine(c, adc));
    TEST_ASSERT_TRUE(t >= last);
    last = t;
  }
  // and the two ends are not the same number, which a stuck polynomial would
  // satisfy the monotonicity above with
  TEST_ASSERT_TRUE(Bme280::temperatureCentiC(Bme280::tFine(c, 0xFFFFF)) >
                   Bme280::temperatureCentiC(Bme280::tFine(c, 0)));
}

// The three readings are computed from one t_fine on purpose: pressure and
// humidity are both functions of the temperature at the moment of conversion.
// So a different t_fine has to move them, or they are not compensated at all.
static void test_t_fine_moves_pressure_and_humidity() {
  const Bme280::Calibration c = plausible();
  const uint32_t pCold = Bme280::pressureCentiPa(c, 100000, 415148);
  const uint32_t pWarm = Bme280::pressureCentiPa(c, 160000, 415148);
  TEST_ASSERT_NOT_EQUAL(pCold, pWarm);
  const uint32_t hCold = Bme280::humidityQ1024(c, 100000, 30000);
  const uint32_t hWarm = Bme280::humidityQ1024(c, 160000, 30000);
  TEST_ASSERT_NOT_EQUAL(hCold, hWarm);
}

// ---------------------------------------------------------------------------
// An authority outside this repository

// The datasheet publishes a worked example for exactly this purpose — a fixed
// set of coefficients, one raw count each, and the answers — and the
// coefficients in plausible() above are that set. So the readings can be
// pinned absolutely after all, which is worth more than everything above it:
// the structural assertions all survive a divisor that is out by a factor of
// two, and this one does not.
//
// Two notes on the numbers. The published t_fine is 128422.29, which is the
// *floating-point* intermediate; the integer path lands one least significant
// bit away at 128423, because it rounds where the other divides. And the
// published pressure is 100653.27 Pa against this path's 100653.28 — one
// hundredth of a pascal, from the same source of difference. The temperature
// matches to the last digit either way.
static void test_the_datasheets_worked_example() {
  const Bme280::Calibration c = plausible();
  const int32_t tf = Bme280::tFine(c, 519888);
  // one LSB of the published 128422.29
  TEST_ASSERT_INT32_WITHIN(2, 128422, tf);
  // 25.08 C, exactly as published
  TEST_ASSERT_EQUAL_INT32(2508, Bme280::temperatureCentiC(tf));
  // 100653.27 Pa as published, in hundredths and within one pascal
  const uint32_t p = Bme280::pressureCentiPa(c, tf, 415148);
  TEST_ASSERT_UINT32_WITHIN(100, 10065327U, p);
}

// ---------------------------------------------------------------------------
// The same function, written twice

// The datasheet gives the compensation twice — the integer path this firmware
// uses, and a floating-point one — and they are not the same expressions: the
// integer path divides by 2048 and 8589934592 where this one divides by 1024
// and 32768, and shifts where this one multiplies. Two transcriptions of two
// different forms agreeing is evidence neither has a divisor out by a factor
// of two, which is the error the structural tests above cannot see and which a
// clamp will happily accept.
//
// This is not the polynomial agreeing with itself. It is the same specification
// implemented from a different set of constants, and a slip in either one
// shows up here.
namespace ref {
double tFine(const Bme280::Calibration& c, uint32_t adcT) {
  const double v1 = ((double)adcT / 16384.0 - (double)c.t1 / 1024.0) * (double)c.t2;
  const double x  = (double)adcT / 131072.0 - (double)c.t1 / 8192.0;
  return v1 + x * x * (double)c.t3;
}
double tempC(double tf) { return tf / 5120.0; }
double pressurePa(const Bme280::Calibration& c, double tf, uint32_t adcP) {
  double v1 = tf / 2.0 - 64000.0;
  double v2 = v1 * v1 * (double)c.p6 / 32768.0;
  v2 = v2 + v1 * (double)c.p5 * 2.0;
  v2 = v2 / 4.0 + (double)c.p4 * 65536.0;
  v1 = ((double)c.p3 * v1 * v1 / 524288.0 + (double)c.p2 * v1) / 524288.0;
  v1 = (1.0 + v1 / 32768.0) * (double)c.p1;
  if (v1 == 0.0) return 0.0;
  double p = 1048576.0 - (double)adcP;
  p = (p - v2 / 4096.0) * 6250.0 / v1;
  v1 = (double)c.p9 * p * p / 2147483648.0;
  v2 = p * (double)c.p8 / 32768.0;
  return p + (v1 + v2 + (double)c.p7) / 16.0;
}
double humidityPct(const Bme280::Calibration& c, double tf, uint32_t adcH) {
  double v = tf - 76800.0;
  v = ((double)adcH - ((double)c.h4 * 64.0 + (double)c.h5 / 16384.0 * v)) *
      ((double)c.h2 / 65536.0 * (1.0 + (double)c.h6 / 67108864.0 * v *
       (1.0 + (double)c.h3 / 67108864.0 * v)));
  v = v * (1.0 - (double)c.h1 * v / 524288.0);
  return v < 0.0 ? 0.0 : (v > 100.0 ? 100.0 : v);
}
} // namespace ref

// Float assertions rather than double ones: Unity's host build compiles the
// double family out, and float resolution at a hundred kilopascals is about
// eight thousandths of a pascal — three orders of magnitude inside the loosest
// tolerance here.
//
// The tolerances are the measured quantisation between the two paths across
// this sweep — 0.014 C, 0.40 Pa and 0.007 % — rounded up with room to spare,
// and they are still two orders of magnitude tighter than any factor-of-two
// slip could hide in.
static void test_the_two_forms_of_the_polynomial_agree() {
  const Bme280::Calibration c = plausible();
  for (uint32_t at = 200000; at <= 900000; at += 7777) {
    const int32_t tfi = Bme280::tFine(c, at);
    const double  tfd = ref::tFine(c, at);
    const double  td  = ref::tempC(tfd);
    if (td > -39.0 && td < 84.0) {                 // away from the clamps
      TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)td, (float)Bme280::temperatureCentiC(tfi) / 100.0f);
    }
    for (uint32_t ap = 300000; ap <= 500000; ap += 9999) {
      const double pd = ref::pressurePa(c, tfd, ap);
      if (pd > 31000.0 && pd < 109000.0) {
        TEST_ASSERT_FLOAT_WITHIN(5.0f, (float)pd, (float)Bme280::pressureCentiPa(c, tfi, ap) / 100.0f);
      }
    }
    for (uint32_t ah = 10000; ah <= 40000; ah += 3333) {
      const double hd = ref::humidityPct(c, tfd, ah);
      if (hd > 0.5 && hd < 99.5) {
        TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)hd, (float)Bme280::humidityQ1024(c, tfi, ah) / 1024.0f);
      }
    }
  }
}

// And the saturation itself, which "never above one hundred" does not pin: a
// cap set to the wrong constant is still never above it. This asserts the
// number, from the far side.
static void test_humidity_saturates_at_exactly_one_hundred_percent() {
  const Bme280::Calibration c = plausible();
  const int32_t tf = Bme280::tFine(c, 519888);
  // Just short of saturation the reading is still under the cap...
  TEST_ASSERT_TRUE(Bme280::humidityQ1024(c, tf, 37442) < 102400U);
  // ...and from here up it is exactly the cap and never a count more.
  TEST_ASSERT_EQUAL_UINT32(102400U, Bme280::humidityQ1024(c, tf, 37443));
  TEST_ASSERT_EQUAL_UINT32(102400U, Bme280::humidityQ1024(c, tf, 0xFFFF));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_twenty_bit_count_is_msb_lsb_and_the_top_nibble);
  RUN_TEST(test_the_unused_nibble_of_the_xlsb_is_not_data);
  RUN_TEST(test_humidity_is_sixteen_bits_of_two_registers);
  RUN_TEST(test_full_scale_counts_decode_to_their_limits);
  RUN_TEST(test_calibration_is_little_endian_and_the_signed_ones_are_signed);
  RUN_TEST(test_h4_and_h5_split_the_shared_byte_low_nibble_to_h4);
  RUN_TEST(test_a_negative_h4_stays_negative_through_the_shift);
  RUN_TEST(test_h6_is_signed);
  RUN_TEST(test_temperature_is_clamped_to_the_specified_range);
  RUN_TEST(test_pressure_is_clamped_to_the_specified_range);
  RUN_TEST(test_humidity_never_exceeds_one_hundred_percent);
  RUN_TEST(test_a_zero_p1_returns_the_floor_instead_of_dividing_by_it);
  RUN_TEST(test_an_all_zero_calibration_does_not_fault);
  RUN_TEST(test_a_higher_count_reads_hotter);
  RUN_TEST(test_t_fine_moves_pressure_and_humidity);
  RUN_TEST(test_the_datasheets_worked_example);
  RUN_TEST(test_the_two_forms_of_the_polynomial_agree);
  RUN_TEST(test_humidity_saturates_at_exactly_one_hundred_percent);
  return UNITY_END();
}
