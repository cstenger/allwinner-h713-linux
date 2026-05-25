/*
 * Unit tests for pure CalculateGamma API (no hardware access).
 */
#include "pq_calculate_gamma.h"

#include <cstdio>

using namespace hy310::pqgamma;

static int failures = 0;

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); ++failures; } \
    else { std::printf(" OK  %s:%d\n", __func__, __LINE__); } \
} while (0)

static std::array<int16_t, 33> identity_cp() {
    std::array<int16_t, 33> cp{};
    for (size_t i = 0; i < cp.size(); ++i)
        cp[i] = static_cast<int16_t>((i * 4095) / 32);
    cp[0] = 0;
    cp[32] = 4095;
    return cp;
}

static void test_basic_shape() {
    GammaInput in{};
    in.ucGammaFactor = 3; // ~2.2 reference

    auto out = calculate_gamma(in, identity_cp());

    CHECK(out.cm_cvbs[0] == 0, "lut[0]=%d", out.cm_cvbs[0]);
    CHECK(out.cm_cvbs[1023] == 4095, "lut[1023]=%d", out.cm_cvbs[1023]);
    CHECK(out.cm_cvbs[512] > 1800 && out.cm_cvbs[512] < 2300,
          "lut[512]=%d expected around 2048", out.cm_cvbs[512]);
}

static void test_gamma_darkens_midtones() {
    auto cp = identity_cp();

    GammaInput g22{};
    g22.ucGammaFactor = 3; // ~2.2 => exponent ratio 1.0
    auto out22 = calculate_gamma(g22, cp);

    GammaInput g_hi{};
    g_hi.ucGammaFactor = 4; // highest stock-mapped entry in dword_4A50
    auto out_hi = calculate_gamma(g_hi, cp);

    CHECK(out_hi.cm_cvbs[512] < out22.cm_cvbs[512],
          "midtone compare high=%d ref=%d",
          out_hi.cm_cvbs[512], out22.cm_cvbs[512]);
}

static void test_all_six_outputs_present() {
    GammaInput in{};
    in.ucGammaFactor = 3;

    auto out = calculate_gamma(in, identity_cp());

    CHECK(out.cm_cvbs[1023] == 4095, "cvbs end=%d", out.cm_cvbs[1023]);
    CHECK(out.cm_hdmi[1023] == 4095, "hdmi end=%d", out.cm_hdmi[1023]);
    CHECK(out.cm_svd[1023] == 4095, "svd end=%d", out.cm_svd[1023]);
    CHECK(out.cm_type_3[1023] == 4095, "type3 end=%d", out.cm_type_3[1023]);
    CHECK(out.cm_type_4[1023] == 4095, "type4 end=%d", out.cm_type_4[1023]);
    CHECK(out.cm_type_5[1023] == 4095, "type5 end=%d", out.cm_type_5[1023]);
}

// ---- Audit-added assertions (item 10) -----------------------------------

static void test_channel_diff_at_colortemp() {
    // Item 10a: cm_cvbs must differ from cm_hdmi at index 512 by >= 50 LSBs
    // when ucColorTemp != 0 (confirms real RGB balance per ColorTemp).
    GammaInput in{};
    in.ucGammaFactor = 3;
    in.ucColorTemp   = 1;  // "Cool" bank — R-bias 0.96, B-bias 1.06
    auto out = calculate_gamma(in, identity_cp());
    int diff = out.cm_cvbs[512] - out.cm_hdmi[512];
    if (diff < 0) diff = -diff;
    CHECK(diff >= 50, "cvbs=%d hdmi=%d diff=%d (need >=50)",
          out.cm_cvbs[512], out.cm_hdmi[512], diff);
}

static void test_low_gamma_brightens_midtones() {
    // Item 10b: factor=0 (->180 ->exponent 0.818) midtone LUT[512] must be
    // HIGHER than factor=4 (->240 ->exponent 1.091, darker). Matches decomp
    // L672: v94 = v22/2.2; pow(x/max, v94). Lower exponent -> brighter mids.
    auto cp = identity_cp();
    GammaInput lo{}; lo.ucGammaFactor = 0;
    GammaInput hi{}; hi.ucGammaFactor = 4;
    auto out_lo = calculate_gamma(lo, cp);
    auto out_hi = calculate_gamma(hi, cp);
    CHECK(out_lo.cm_cvbs[512] > out_hi.cm_cvbs[512],
          "lo(factor=0)[512]=%d hi(factor=4)[512]=%d (lo must be brighter)",
          out_lo.cm_cvbs[512], out_hi.cm_cvbs[512]);
}

int main() {
    test_basic_shape();
    test_gamma_darkens_midtones();
    test_all_six_outputs_present();
    test_channel_diff_at_colortemp();
    test_low_gamma_brightens_midtones();

    std::printf("\n%s\n", failures ? "FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

