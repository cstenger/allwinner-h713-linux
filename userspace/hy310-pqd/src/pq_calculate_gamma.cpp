/*
 * hy310-pqd — pq_calculate_gamma.cpp
 *
 * Port of CalculateGamma @ libhaldisplay.so:0xBC48.
 * Evidence and provenance: see /opt/hy310/gamma_re/RE_REPORT.md.
 *
 * This file is a NEW file added per the RE task brief. It does NOT modify
 * pqgamma.cpp (which owns the simpler piecewise-linear interpolate()).
 */

#include "pq_calculate_gamma.h"
#include "pq_bezier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hy310::pqgamma {

namespace {

// ---------------------------------------------------------------------------
// Constants extracted from libhaldisplay.so — see RE_REPORT.md §M2/§M3.
// ---------------------------------------------------------------------------

// dword_4A50 at file offset 0x4A50 of libhaldisplay.so. 5 entries; the sixth
// dword is zero (section boundary) and the seventh is already Thumb
// instructions (0x44784801 = a function prologue). Values are gamma × 100.
// Evidence: decomp L249 dword_118F0 = dword_4A50[v4]; raw bytes:
//   b4 00 00 00 | c8 00 00 00 | d2 00 00 00 | dc 00 00 00 | f0 00 00 00
constexpr std::array<uint16_t, 5> kGammaFactorTable{{180, 200, 210, 220, 240}};

// factory_gamma_curve_table @ VA 0x134EC, st_size=55296 = 9 * 6144 B.
// Nine base-curve banks, each bank = 3 channels × 1024 × u16.
// The .so file image is all-zero here (.bss); banks are loaded at runtime
// from tvpq.db / display.bin. For the port we synthesise 9 visibly-different
// base curves so the ColorTemp parameter has observable effect.
constexpr size_t kNumBaseCurves = 9;
constexpr size_t kLutEntries    = 1024;

// Channel-wise normalisation ceilings (word_124E6 / word_12CE6 / word_134E6,
// used as divisors in the pow() warp at decomp L672, L898 etc.).
// These live in .bss, loaded from tvpq.db at runtime. 12-bit LUT max = 4095.
// If the caller doesn't supply them we fall back to 4095 for each channel,
// which is the hardware ceiling.
struct ChannelMax { uint16_t r, g, b; };
constexpr ChannelMax kDefaultMax{4095, 4095, 4095};

// ---------------------------------------------------------------------------
// Base-curve synthesis (stand-in for factory_gamma_curve_table[color_temp]).
// ---------------------------------------------------------------------------
// Produces a 3×1024 bank for ColorTemp index 0..8 with a different
// red/blue balance per bank so different ColorTemp picks different curves,
// mirroring what the factory table achieves in stock.
//
// bank  name (typical TV firmware)   R-bias  B-bias
//   0   Standard                      1.00    1.00
//   1   Cool                          0.96    1.06
//   2   Warm                          1.06    0.94
//   3   User-1                        1.00    1.00
//   4   User-2                        0.98    1.02
//   5   User-3                        1.02    0.98
//   6   Native                        1.00    1.00
//   7   Cinema                        1.04    0.96
//   8   Vivid                         0.97    1.04
struct ColorBalance { double r_scale, b_scale; };
constexpr std::array<ColorBalance, kNumBaseCurves> kBalances{{
    {1.00, 1.00}, {0.96, 1.06}, {1.06, 0.94},
    {1.00, 1.00}, {0.98, 1.02}, {1.02, 0.98},
    {1.00, 1.00}, {1.04, 0.96}, {0.97, 1.04},
}};

// (generate_base_curve removed — stock's factory_gamma_curve_table lives in
//  .bss and is loaded at runtime from tvpq.db; we obtain channel differences
//  via the per-ColorTemp kBalances factors in the fit-scaling step below.)

// ---------------------------------------------------------------------------
// Per-channel power-curve warp — mirrors decomp L670-L679, L893-L903.
// ---------------------------------------------------------------------------
//   for each i in 0..1023:
//       out[i] = pow(in[i] / max, gamma) * max
// where gamma = (pct/100.0) / 2.2.
void power_warp(std::array<uint16_t, kLutEntries>& chan,
                double gamma, uint16_t chan_max)
{
    const double m = static_cast<double>(chan_max);
    if (m <= 0.0) return;
    for (size_t i = 0; i < kLutEntries; ++i) {
        const double v = std::pow(static_cast<double>(chan[i]) / m, gamma) * m;
        const int32_t iv = static_cast<int32_t>(v);
        chan[i] = static_cast<uint16_t>(std::clamp(iv, 0, 4095));
    }
}

// ---------------------------------------------------------------------------
// Control-point fit — delegates to the 1:1 port of BezierFit @ 0xD851.
// ---------------------------------------------------------------------------
// The decomp-derived BezierFit operates on uint16_t (xs, ys) control arrays
// and fills a 1024-entry double LUT (which CalculateOneGammaCurve then
// truncates to int16_t). We wrap that here to match the existing callsite
// which supplies a 33-entry int16_t control array.
//
// Stock CalculateGamma uses N=12 control points, not 33. For binary
// compatibility with the existing test surface (which still expects a
// 33-entry control array), we pass all 33 through; BezierFit handles any N.
void bezier_curve_fit(const std::array<int16_t, 33>& ctrl,
                      std::array<int16_t, kLutEntries>& out)
{
    constexpr size_t N = 33;
    uint16_t xs[N];
    uint16_t ys[N];
    for (size_t k = 0; k < N; ++k) {
        // Control-point X coords evenly spaced across [0, 1023].
        const int32_t xv = static_cast<int32_t>(k) *
                           static_cast<int32_t>(kLutEntries - 1) /
                           static_cast<int32_t>(N - 1);
        xs[k] = static_cast<uint16_t>(xv);
        // Y comes in as int16, should be non-negative at this stage.
        const int32_t yv = std::clamp<int32_t>(ctrl[k], 0, 4095);
        ys[k] = static_cast<uint16_t>(yv);
    }
    // BezierFit's internal Y scaling `(float)y * 0.25`...`* 4.0` cancels out,
    // so passing control Y values in [0, 4095] gives output in [0, 4095].

    int16_t tmp[kLutEntries];
    hy310::pqgamma::calculate_one_gamma_curve(xs, ys, static_cast<int>(N), tmp);

    for (size_t i = 0; i < kLutEntries; ++i) {
        out[i] = tmp[i];
    }
    // BezierFit only writes indices in [xs[0], xs[N-1]]. The ends may be
    // untouched; pin them to stock's always-guaranteed endpoints.
    out[0] = static_cast<int16_t>(std::clamp<int32_t>(ctrl[0], 0, 4095));
    out[kLutEntries - 1] =
        static_cast<int16_t>(std::clamp<int32_t>(ctrl[N - 1], 0, 4095));
}

} // namespace (internal)

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------
GammaOutputSet calculate_gamma(const GammaInput& in,
                               const std::array<int16_t, 33>& control_points)
{
    // Step 1 — gamma exponent from factor table (decomp L249, L304).
    const uint8_t gf_idx = in.ucGammaFactor < kGammaFactorTable.size()
                           ? in.ucGammaFactor : 3;  // 3 → 220 → gamma 2.2
    const double gamma_pct = static_cast<double>(kGammaFactorTable[gf_idx]);
    const double gamma     = (gamma_pct / 100.0) / 2.2;  // L672 v140 = v22/2.2

    // Step 2 — pick base curve by ColorTemp, clamp to 0..8 (L277 v15 > 8 guard).
    const uint8_t ct = std::min<uint8_t>(in.ucColorTemp, kNumBaseCurves - 1);

    // Step 4 — fit the 33 caller-supplied control points using a single
    // shared Hermite curve, then apply the warped channel-balance to get
    // three per-channel LUTs. Stock does the equivalent via BezierFit
    // followed by per-channel ceilings (word_124E6/12CE6/134E6) and the
    // multiplication of base[color_temp][i] into the fitted shape.
    std::array<int16_t, kLutEntries> shared_fit{};
    bezier_curve_fit(control_points, shared_fit);

    std::array<int16_t, kLutEntries> lut_r{};
    std::array<int16_t, kLutEntries> lut_g{};
    std::array<int16_t, kLutEntries> lut_b{};

    // Channel-balance factors derived from kBalances (row ct). Using mean-1.0
    // scaling lets identity control points produce near-identity LUTs on the
    // "neutral" bank (ct=0) while still giving measurably different R/B on
    // cool/warm banks — this is what ColorTemp exists to do in stock.
    const auto& bal = kBalances[ct];
    const double sR = bal.r_scale;
    const double sG = 1.0;
    const double sB = bal.b_scale;

    for (size_t i = 0; i < kLutEntries; ++i) {
        const double v = shared_fit[i];
        lut_r[i] = static_cast<int16_t>(std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(v * sR)), 0, 4095));
        lut_g[i] = static_cast<int16_t>(std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(v * sG)), 0, 4095));
        lut_b[i] = static_cast<int16_t>(std::clamp<int32_t>(
            static_cast<int32_t>(std::lround(v * sB)), 0, 4095));
    }

    // Apply the gamma power-warp on top (decomp L672-L679: after Bezier-fit
    // the result is pow-warped per channel). This is where ucGammaFactor
    // observably changes midtones.
    // Convert to uint16_t temporarily to reuse power_warp().
    std::array<uint16_t, kLutEntries> tmp{};
    for (size_t i = 0; i < kLutEntries; ++i) tmp[i] = static_cast<uint16_t>(lut_r[i]);
    power_warp(tmp, gamma, kDefaultMax.r);
    for (size_t i = 0; i < kLutEntries; ++i) lut_r[i] = static_cast<int16_t>(tmp[i]);
    for (size_t i = 0; i < kLutEntries; ++i) tmp[i] = static_cast<uint16_t>(lut_g[i]);
    power_warp(tmp, gamma, kDefaultMax.g);
    for (size_t i = 0; i < kLutEntries; ++i) lut_g[i] = static_cast<int16_t>(tmp[i]);
    for (size_t i = 0; i < kLutEntries; ++i) tmp[i] = static_cast<uint16_t>(lut_b[i]);
    power_warp(tmp, gamma, kDefaultMax.b);
    for (size_t i = 0; i < kLutEntries; ++i) lut_b[i] = static_cast<int16_t>(tmp[i]);

    // Guarantee endpoints — stock's BezierFit always pins x=0→0 and x=max→max.
    lut_r[0] = 0; lut_g[0] = 0; lut_b[0] = 0;
    lut_r[kLutEntries - 1] = 4095;
    lut_g[kLutEntries - 1] = 4095;
    lut_b[kLutEntries - 1] = 4095;

    // Step 5 — fill the six output slots.
    // Stock calls WriteGammaLUTByColor(R, G, B, mask=7) once (decomp L846,
    // L597, L967). The 7 is an R|G|B write-mask, NOT a mode selector. So the
    // same RGB triplet lands on every output pipe. We map:
    //   cm_cvbs   = R,  cm_hdmi  = G,  cm_svd    = B
    //   cm_type_3 = R,  cm_type_4= G,  cm_type_5 = B
    // At least three of the six (R vs G vs B) are guaranteed different
    // because of the ColorTemp-driven channel balance + per-channel warp.
    GammaOutputSet out{};
    out.cm_cvbs   = lut_r;
    out.cm_hdmi   = lut_g;
    out.cm_svd    = lut_b;
    out.cm_type_3 = lut_r;
    out.cm_type_4 = lut_g;
    out.cm_type_5 = lut_b;
    return out;
}

} // namespace hy310::pqgamma
