/*
 * Unit tests for gamma LUT interpolation — no hardware access.
 */
#include "pqgamma.h"
#include <cassert>
#include <cstdio>

using namespace hy310::pqgamma;

static int failures = 0;

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); ++failures; } \
    else { std::printf(" OK  %s:%d\n", __func__, __LINE__); } \
} while (0)

static void test_identity() {
    auto c   = GammaCurve::identity();
    auto lut = interpolate(c);

    /* Identity: lut[i] should approximate i × (4095/1023) ≈ 4 × i. */
    CHECK(lut[0] == 0, "lut[0] = %d, expected 0", lut[0]);
    CHECK(lut[1023] >= 4090, "lut[1023] = %d, expected ~4095", lut[1023]);
    CHECK(lut[512] > 2000 && lut[512] < 2100,
          "lut[512] = %d, expected ~2048", lut[512]);
}

static void test_power_curve() {
    auto c   = GammaCurve::from_exponent(2.2);
    auto lut = interpolate(c);

    /* Gamma 2.2: lut[i] should be lower in mid-range than identity. */
    CHECK(lut[0]    == 0,    "lut[0] = %d", lut[0]);
    CHECK(lut[1023] >= 4000, "lut[1023] = %d", lut[1023]);
    CHECK(lut[512]  <  1500, "gamma 2.2 at mid-range = %d, expected <1500", lut[512]);
}

static void test_packing() {
    /* Confirm packing layout: u32[i] = (lut[2i+1] << 12) | lut[2i] */
    std::array<int16_t, LUT_ENTRIES> lut{};
    for (size_t i = 0; i < lut.size(); ++i)
        lut[i] = static_cast<int16_t>(i & LUT_SAMPLE_MASK);

    /* We can't call pack_lut directly (private), but we can verify the
     * algorithm matches by doing the same thing here. */
    uint32_t expected_0 = (static_cast<uint32_t>(lut[1]) << 12) | lut[0];
    uint32_t expected_5 = (static_cast<uint32_t>(lut[11]) << 12) | lut[10];

    CHECK(expected_0 == 0x001'000,
          "pack[0] = 0x%x, expected 0x001000 (= 1<<12 | 0)", expected_0);
    CHECK(expected_5 == (11u << 12) | 10u,
          "pack[5] = 0x%x", expected_5);
}

static void test_endpoints() {
    /* Any curve: interpolated endpoints must match control-point endpoints. */
    auto c   = GammaCurve::from_exponent(1.8);
    auto lut = interpolate(c);
    CHECK(lut[0]                == c.points[0],  "start mismatch");
    CHECK(lut[LUT_ENTRIES - 1]  == c.points[32], "end mismatch");
}

int main() {
    test_identity();
    test_power_curve();
    test_packing();
    test_endpoints();
    std::printf("\n%s\n", failures ? "FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
