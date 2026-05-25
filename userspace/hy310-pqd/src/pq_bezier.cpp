/*
 * hy310-pqd — pq_bezier.cpp
 *
 * 1:1 C++ port of the four Bezier-fit math routines from libhaldisplay.so.
 * Evidence: /opt/hy310/gamma_re/solvers_decomp.txt.
 *
 * All non-trivial numeric constants in this file trace to a specific line
 * of the Hex-Rays decompilation listed in that evidence file; see inline
 * comments citing decomp line references.
 */

#include "pq_bezier.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hy310 {
namespace pqgamma {

// ---------------------------------------------------------------------------
// Thread-local mirrors of the libhaldisplay.so globals that participate in
// the cubic / Newton solve. Names and byte layout match stock:
//
//   Solve_Order3_Equation writes its argument 5-tuple (a, b, c, d, <used>)
//   into four file-scope doubles named (in Hex-Rays output) a3, a2, a1, a0.
//   Newton_Solve (a standalone function) reads those same globals to
//   evaluate f(x) and f'(x). We preserve the calling convention by keeping
//   them here as file-scope `thread_local`.
//
//   BezierPt1..4 @ .bss 0x20CF0..0x20D30 are NOT const templates. They are
//   working variables (.x, .y doubles). See decomp:
//       BezierPt1   = Dot2[0]; dbl_20CF8 = Dot2[1];   // the 2nd control pt
//       BezierPt4   = Dot3[0]; dbl_20D28 = Dot3[1];   // the 3rd control pt
//       BezierPt2   = v38;     dbl_20D08 = ...        // handle 1
//       BezierPt3   = v39;     dbl_20D18 = ...        // handle 2
// ---------------------------------------------------------------------------
namespace {

// Cubic coefficients shared with Newton_Solve (stock globals a3, a2, a1, a0).
thread_local double g_coef_a3;  // coefficient of x^3 (stock ::a3, = caller a)
thread_local double g_coef_a2;  // coefficient of x^2 (stock ::a2, = caller b)
thread_local double g_coef_a1;  // coefficient of x^1 (stock ::a1, = caller c)
thread_local double g_coef_a0;  // constant term      (stock a0,   = caller d)

// Bezier control points (two doubles each = x, y). Working variables.
struct Pt { double x, y; };
thread_local Pt g_bez_p1;   // stock BezierPt1 / dbl_20CF8
thread_local Pt g_bez_p2;   // stock BezierPt2 / dbl_20D08
thread_local Pt g_bez_p3;   // stock BezierPt3 / dbl_20D18
thread_local Pt g_bez_p4;   // stock BezierPt4 / dbl_20D28

// Stock divides Y input by 4 up front and multiplies the Bezier output by 4
// at the end (decomp L... `v17 = (float)*v13 * 0.25` and `v57 * 4.0`).
// Keep the magic numbers out of the math kernel.
constexpr double kYScaleIn   = 0.25;   // decomp: `(float)*v13 * 0.25`
constexpr double kYScaleOut  = 4.0;    // decomp: `* 4.0` before clamp
constexpr double kMaxY       = 4095.0; // decomp: `if ( v57 > 4095.0 ) v57 = 4095.0;`
constexpr double kSix        = 6.0;    // decomp initial: `v8 = 6.0;`
constexpr double kThree      = 3.0;    // decomp literal  `3.0`
constexpr double kHalf       = 0.5;    // decomp literal  `0.5` (midpoint)

} // anonymous namespace

// ---------------------------------------------------------------------------
// Solve_Order2_Equation @ 0xCF91.
//
// Decomp summary:
//   Initialize result struct to {0, -1.0, 0, -1.0, 0, -1.0, count=0}.
//   if ( a == 0 ) {
//       if ( b == 0 ) return;                  // degenerate 0=c
//       if ( c == 0 ) { r0=r1=0, count=2; }    // bx=0 => x=0 (double root)
//       else          { r0=r1=-c/b, count=2 }; // linear
//   }
//   D = b*b - 4ac;
//   if ( D < 0 )  return (count still 0)
//   if ( D == 0 ) r0=r1=-b/(2a), count=2
//   else          r0 = (-b - sqrt D) / 2a, r1 = (-b + sqrt D) / 2a, sorted
// ---------------------------------------------------------------------------
QuadRoots solve_order2(double a, double b, double c)
{
    QuadRoots out{};
    // Stock initial values written to the result struct:
    //   offset  0:  double 0.0        (r0)
    //   offset  8:  double 0.0 hi = -1074790400 => -1.0 ... wait: the decomp
    //     actually writes (_DWORD*)(r+4) = -1074790400 and (_DWORD*)(r+12) = -1074790400,
    //     which are the HIGH halves of doubles at offsets 0 and 8 respectively.
    //     0xBFF00000 == -1074790400 signed. That encodes -1.0 (sign + exp).
    //   offset 16:  double similarly initialised to -1.0 but the LOW word is 0.
    //   offset 24:  byte count = 0.
    out.r0 = -1.0;
    out.r1 = -1.0;
    out.r2 = -1.0;
    out.count = 0;

    if (a == 0.0) {
        if (b == 0.0) {
            return out;           // degenerate: 0 = c, no roots reported
        }
        if (c == 0.0) {
            // Stock: writes 0 into r0/r1, count=2, then returns.
            out.r0 = 0.0;
            out.r1 = 0.0;
            out.count = 2;
            return out;
        }
        const double r = -c / b;  // decomp: v6 = -a4 / a3
        out.r0 = r;
        out.r1 = r;
        out.count = 2;
        return out;
    }

    const double disc = b * b + a * -4.0 * c;   // decomp L: `a3*a3 + a2*-4.0*a4`
    if (disc < 0.0) {
        return out;
    }
    if (disc == 0.0) {
        const double r = b * -0.5 / a;          // decomp: `a3 * -0.5 / a2`
        out.r0 = r;
        out.r1 = r;
        out.count = 2;
        return out;
    }

    const double sq = std::sqrt(disc);
    const double rA = (-b - sq) * 0.5 / a;
    const double rB = ( sq - b) * 0.5 / a;
    // Decomp sorts: smaller root -> r0.
    if (rB >= rA) {
        out.r0 = rA;
        out.r1 = rB;
    } else {
        out.r0 = rB;
        out.r1 = rA;
    }
    out.count = 2;
    return out;
}

// ---------------------------------------------------------------------------
// Newton_Solve @ 0xCE71.
//
// Uses globally-stored cubic coefficients g_coef_a3..a0. Pre-check: if the
// residual is already <= 1e-5, return x unchanged. Loop bound: i <= 0x7C*4+3
// = 499 iterations (decomp condition `i >> 2 <= 0x7C`).
// ---------------------------------------------------------------------------
double newton_solve(double x0)
{
    // decomp: if ( fabs(a0 + ::a1*x + a3*x^3 + a2*x^2) <= 0.00001 ) return x
    double x = x0;
    if (std::fabs(g_coef_a0 + g_coef_a1 * x + g_coef_a3 * x * x * x +
                  g_coef_a2 * x * x) <= 0.00001) {
        return x;
    }

    for (unsigned int i = 0; (i >> 2) <= 0x7Cu; ++i) {
        // f'(x) = 3*a3*x^2 + 2*a2*x + a1
        // (decomp expresses it as: ::a1 + (a2+a2)*x + x*(a3*3.0*x) )
        const double fp = g_coef_a1
                        + (g_coef_a2 + g_coef_a2) * x
                        + x * (g_coef_a3 * 3.0 * x);
        if (fp == 0.0) break;
        // x -= f(x) / f'(x)
        // decomp: a1 = a1 - (a0 + ::a1*a1 + a1*(a2*a1) + a1*(a1*(a3*a1))) / v3
        x = x - (g_coef_a0 + g_coef_a1 * x + x * (g_coef_a2 * x) +
                 x * (x * (g_coef_a3 * x))) / fp;
        if (std::fabs(g_coef_a0 + g_coef_a1 * x + x * (g_coef_a2 * x) +
                      x * (x * (g_coef_a3 * x))) <= 0.00001) {
            return x;
        }
    }
    return -1.0;
}

// ---------------------------------------------------------------------------
// Helper replicating the inline Newton block that appears 4 times inside
// Solve_Order3_Equation (decomp labels LABEL_29 / 60 / 69 / 78). Signature
// differs from newton_solve() only in that the globals are already set by
// the caller. Kept separate so the Newton_Solve port is a pure 1:1 symbol.
// ---------------------------------------------------------------------------
namespace {

// Evaluate f(x) = a0 + a1*x + a2*x^2 + a3*x^3 using the global coeffs, in
// the exact nested-multiply order of the decomp to reproduce its FP rounding.
inline double cubic_f(double x) {
    return x * g_coef_a1
         + x * (x * g_coef_a2)
         + x * (x * (x * g_coef_a3))
         + g_coef_a0;
}

// Newton inline, mirroring the 4 copies inside Solve_Order3_Equation.
// Returns the converged root or -1.0 on failure.
double inline_newton(double x) {
    for (unsigned int iter = 0; (iter >> 2) <= 0x7Cu; ++iter) {
        // decomp: v21 = v15*x + x*(v14*x) + a4;
        //         where v15 = 2*a3 (i.e. 2b), v14 = 3*a2 (i.e. 3a), a4 = c
        //         i.e. f'(x) = 3 a x^2 + 2 b x + c
        const double fp = (g_coef_a2 + g_coef_a2) * x
                        + x * ((g_coef_a3 * 3.0) * x)
                        + g_coef_a1;
        if (fp == 0.0) break;
        x = x - cubic_f(x) / fp;
        if (std::fabs(cubic_f(x)) <= 0.00001) {
            return x;
        }
    }
    return -1.0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Solve_Order3_Equation @ 0xD089.
//
// Stock approach (reverse-engineered from decomp):
//
//   1. Copy (a,b,c,d) into the four shared globals (g_coef_a3..a0).
//      NB: stock's Hex-Rays names them `::a3 = a2; ::a2 = a3; ::a1 = a4; a0 = a5`
//      so the mapping is: stock-global a3 == CUBIC  coefficient (x^3)
//                         stock-global a2 == SQUARE coefficient (x^2)
//                         stock-global a1 == LINEAR coefficient (x^1)
//                         stock-global a0 == CONST  term
//   2. Initialise output struct to {0, -1.0, 0, -1.0, 0, -1.0, count=0}.
//   3. If d == 0: one root is 0; solve quadratic on the residual.
//   4. If a == 0: reduce to quadratic a·b x^2 + c x + d = 0 (see decomp
//      "if ( a2 == 0.0 )" block near the top).
//   5. Otherwise: evaluate a residual at x=-10000 and at x=+10000 to find a
//      sign change, then run Newton. Follow with two more Newton searches
//      bracketed by the extrema of f'(x) = 0 (the two roots of 2*b x + 3*a x^2
//      + c = 0). Each search is up to 500 iterations.
// ---------------------------------------------------------------------------
CubicRoots solve_order3(double a, double b, double c, double d)
{
    // Publish to shared globals.
    g_coef_a3 = a;
    g_coef_a2 = b;
    g_coef_a1 = c;
    g_coef_a0 = d;

    CubicRoots out{};
    out.r0 = -1.0;
    out.r1 = -1.0;
    out.r2 = -1.0;
    out.count = 0;

    // Case: d == 0 -> one root is x=0, reduce to quadratic a x^2 + b x + c = 0
    if (d == 0.0) {
        QuadRoots q = solve_order2(a, b, c);
        out.r0 = 0.0;
        out.r1 = q.r0;
        out.r2 = q.r1;
        out.count = q.count;
        return out;
    }

    // Case: leading coeff a == 0 -> quadratic b x^2 + c x + d = 0
    if (a == 0.0) {
        // Decomp branches:
        //   if ( a3 == 0.0 ) { linear cx + d = 0 -> x = -d/c, ... }
        //   else            { D = c*c - 4 b d ; etc. }
        if (b == 0.0) {
            if (c != 0.0) {
                const double r = -d / c;
                out.r0 = r;
                out.r1 = r;       // decomp writes r0 = r1 = r, then count=2
                out.count = 2;
            }
            return out;
        }
        const double disc = c * c + b * -4.0 * d;
        if (disc < 0.0) {
            return out;           // decomp returns the all- -1.0 struct
        }
        double rA, rB;
        if (disc == 0.0) {
            rA = c * -0.5 / b;
            rB = rA;
        } else {
            const double sq = std::sqrt(disc);
            const double rX = (sq - c) * 0.5 / b;
            const double rY = (-c - sq) * 0.5 / b;
            rA = std::min(rX, rY);
            rB = std::max(rX, rY);
        }
        // Decomp writes these at offsets 8 and 16 (not 0 and 8), leaving
        // offset 0 as the original -1.0 initialiser. We preserve that.
        out.r0 = -1.0;
        out.r1 = rA;
        out.r2 = rB;
        out.count = 2;
        return out;
    }

    // General cubic. Evaluate at the extrema ±10000 (decomp literal constants
    // `10000.0`). The value `v13` in the decomp is effectively f(-10000):
    //   v13 = 1e8 * b  -  1e12 * a  -  1e4 * c  +  d
    const double f_neg10k = b * 10000.0 * 10000.0
                          - a * 10000.0 * 10000.0 * 10000.0
                          - c * 10000.0
                          + d;
    // v14 = 3a, v15 = 2b   (for f'(x) = 3a x^2 + 2b x + c)
    const double threeA = a * 3.0;
    const double twoB   = b + b;

    // First Newton search: starting at -10000 provided |f(-10000)| > 1e-5.
    double root_left = -10000.0;
    if (std::fabs(f_neg10k) > 0.00001) {
        root_left = -1.0;
        for (unsigned int i = 0; (i >> 2) <= 0x7Cu; ++i) {
            // f'(x) at x = -10000 updated each iter via x
            double x = (i == 0) ? -10000.0 : root_left;
            const double fp = twoB * x + x * (threeA * x) + c;
            if (fp == 0.0) break;
            x = x - (x * c + x * (x * b) + x * (x * (x * a)) + d) / fp;
            root_left = x;
            if (std::fabs(x * c + x * (x * b) + x * (x * (x * a)) + d) <= 0.00001) {
                break;
            }
        }
    }
    out.r0 = root_left;

    // Find the two critical points of f by solving f'(x) = 0
    //   3a x^2 + 2b x + c = 0
    double xl = 0.0, xr = 0.0;
    bool have_crit = false;
    if (threeA == 0.0) {
        if (twoB != 0.0) {
            if (c != 0.0) {
                xl = -c / twoB;
                xr = xl;
            }
            have_crit = true;
        }
    } else {
        const double disc2 = twoB * twoB + threeA * -4.0 * c;
        if (disc2 >= 0.0) {
            if (disc2 == 0.0) {
                xl = twoB * -0.5 / threeA;
                xr = xl;
            } else {
                const double sq2 = std::sqrt(disc2);
                const double cA = (-twoB - sq2) * 0.5 / threeA;
                const double cB = (sq2 - twoB) * 0.5 / threeA;
                xl = std::min(cA, cB);
                xr = std::max(cA, cB);
            }
            have_crit = true;
        }
    }

    if (have_crit) {
        // f at xl
        const double f_xl = xl * c + xl * (xl * b) + xl * (xl * (xl * a)) + d;
        // If sign change between (-10000, xl), Newton from midpoint ((xl-10000)/2).
        if (f_neg10k * f_xl < 0.0) {
            double mid = (xl + -10000.0) * 0.5;
            if (std::fabs(mid * c + mid * (mid * b) + mid * (mid * (mid * a)) + d) > 0.00001) {
                mid = inline_newton(mid);
            }
            out.r0 = mid;
        }
        // f at xr
        const double f_xr = xr * c + xr * (xr * b) + xr * (xr * (xr * a)) + d;
        if (f_xl * f_xr < 0.0) {
            double mid = (xl + xr) * 0.5;
            if (std::fabs(mid * c + mid * (mid * b) + mid * (mid * (mid * a)) + d) > 0.00001) {
                mid = inline_newton(mid);
            }
            out.r1 = mid;
        }
        // f at +10000
        const double f_pos10k = c * 10000.0
                              + a * 10000.0 * 10000.0 * 10000.0
                              + b * 10000.0 * 10000.0
                              + d;
        if (f_pos10k * f_xr < 0.0) {
            double mid = (xr + 10000.0) * 0.5;
            if (std::fabs(mid * c + mid * (mid * b) + mid * (mid * (mid * a)) + d) > 0.00001) {
                mid = inline_newton(mid);
            }
            out.r2 = mid;
        }
    }

    // `count` byte is not updated in this general path in the decomp — stock
    // leaves it at 0 and the caller (BezierFit) only looks at the numeric
    // root values vs. 0.0/1.0. We mirror that.
    return out;
}

// ---------------------------------------------------------------------------
// BezierFit @ 0xD851.
//
// High-level behaviour (reverse-engineered from decomp):
//
//   for each consecutive pair of control points (xs[i], ys[i]) to
//   (xs[i+1], ys[i+1]):
//     1. Fetch "previous", "current", "next", "after" control points
//        (Dot1..Dot4) — at segment boundaries the previous/after clamp to
//        the current/next.
//     2. Compute three chord lengths:
//          v28 = |P1 - P2|   (prev vs current)
//          v29 = |P3 - P4|   (next vs after)
//          v31 = |P2 - P3| * 0.5  (half the current-next chord)
//     3. Choose Bezier control handles P_h1 (between P2 and P3) and P_h2
//        (between P3 and P2) with a scaling heuristic that prevents
//        overshoot (the |v28/6| vs v31 test in the decomp).
//     4. Store the four points in the `g_bez_p*` working variables,
//        derive cubic coefficients a=(P4+3(P2-P3))−P1, b=3(P1−2 P2+P3),
//        c=3(P2−P1), publish them to `g_coef_*`, then:
//     5. For each integer x in (xs[i], xs[i+1]]:
//            solve  a t^3 + b t^2 + c t + (P1.x - x) = 0
//            pick the real root in [0,1],
//            evaluate the cubic-Bezier Y at that t,
//            write (Y * 4.0) clamped to [0, 4095] into out[x].
//        If no real root in [0,1] is found, fall back to linear
//        interpolation between (P1.x, P1.y) and (P4.x, P4.y).
// ---------------------------------------------------------------------------
void bezier_fit(const uint16_t* xs,
                const uint16_t* ys,
                int n_points,
                double* out)
{
    if (n_points < 2) return;

    // `v7 = *xs - 1` in stock => the last-written output index, starts one
    // before the first control-point X so the first `++v7` inside the loop
    // yields xs[0] itself. We keep that idiom.
    int x_cursor = static_cast<int>(xs[0]) - 1;

    const int n_minus_1 = n_points - 1;
    const int n_minus_2 = n_points - 2;

    for (int seg = 0; seg != n_minus_1; ) {
        // --- Pull Dot1..Dot4 (prev, curr, next, after). ---------------------
        double prev_x, prev_y;
        if (seg == 0) {
            prev_x = static_cast<double>(xs[0]);
            prev_y = static_cast<double>(ys[0]) * kYScaleIn;
        } else {
            prev_x = static_cast<double>(xs[seg - 1]);
            prev_y = static_cast<double>(ys[seg - 1]) * kYScaleIn;
        }
        const double curr_x = static_cast<double>(xs[seg]);
        const double curr_y = static_cast<double>(ys[seg]) * kYScaleIn;
        const double next_x = static_cast<double>(xs[seg + 1]);
        const double next_y = static_cast<double>(ys[seg + 1]) * kYScaleIn;
        double after_x, after_y;
        if (seg >= n_minus_2) {
            after_x = next_x;
            after_y = next_y;
        } else {
            after_x = static_cast<double>(xs[seg + 2]);
            after_y = static_cast<double>(ys[seg + 2]) * kYScaleIn;
        }

        // Stock writes: BezierPt1 = Dot2 (curr), BezierPt4 = Dot3 (next),
        // BezierPt2 & BezierPt3 = the two handles computed below.
        g_bez_p1 = {curr_x, curr_y};
        g_bez_p4 = {next_x, next_y};

        // --- Chord lengths & overshoot test. --------------------------------
        const double dx_cn = curr_x - next_x;
        const double dy_cn = curr_y - next_y;
        const double dx_ca = curr_x - after_x;  // v27 later reused
        const double dy_ca = curr_y - after_y;  // v26 later reused

        // Decomp:
        //   v28 = sqrt((Dot1 - Dot3).(Dot1 - Dot3))
        //   v29 = sqrt((Dot2 - Dot4).(Dot2 - Dot4))
        //   v30 = v29 / 6
        //   v31 = sqrt((Dot2 - Dot3).(Dot2 - Dot3)) * 0.5
        // Caveat: the decomp interleaves variables; see lines around
        //   v28 = sqrt((v15 - v22) * (v15 - v22) + (v18 - v23)*(v18-v23))
        //   where v15=prev.x, v22=next.x, v18=prev.y, v23=next.y ; and
        //   v29 uses v27=curr-after, v26=curr_y-after_y.
        const double chord_pn = std::sqrt(
            (prev_x - next_x) * (prev_x - next_x) +
            (prev_y - next_y) * (prev_y - next_y));
        const double chord_ca = std::sqrt(dx_ca * dx_ca + dy_ca * dy_ca);
        const double chord_pn_over6 = chord_pn / kSix;
        const double chord_ca_over6 = chord_ca / kSix;
        const double half_chord_cn = std::sqrt(dx_cn * dx_cn + dy_cn * dy_cn) * kHalf;

        // decomp: v32 = v28/6 < v31; if (v32) v32 = v30 < v31;
        //         i.e. both sides shorter than half the current-next chord.
        const bool both_short = (chord_pn_over6 < half_chord_cn) &&
                                (chord_ca_over6 < half_chord_cn);

        double h1_x, h1_y, h2_x, h2_y;

        if (both_short) {
            // Symmetric / endpoint case — decomp special branch:
            //   v36 = (v15==v19 || v17!=v20) ? 6.0 : 3.0
            //   h1  = (next_x - prev_x)/v36 + curr_x ; corresponding Y
            //   if (after_y == next_y) divisor = 3.0 else 6.0 for h2.
            double div_h1 = kSix;
            if (prev_x == curr_x) div_h1 = kThree;
            if (prev_y != curr_y) div_h1 = kSix;  // decomp: `if ( v17 != v20 ) v36 = v8`

            h1_x = (next_x - prev_x) / div_h1 + curr_x;
            h1_y = (next_y - prev_y) / div_h1 + curr_y;

            const double div_h2 = (after_y == next_y) ? kThree : kSix;
            h2_x = next_x - (after_x - curr_x) / div_h2;
            h2_y = next_y - (after_y - curr_y) / div_h2;
        } else {
            // decomp: scale handles by half_chord / chord_pn or chord_ca
            // whichever governs, so handle length = half_chord_cn.
            const double s_h1 = half_chord_cn / chord_pn;
            const double s_h2 = half_chord_cn / chord_ca;
            h1_x = (next_x - prev_x) * s_h1 + curr_x;
            h1_y = (next_y - prev_y) * s_h1 + curr_y;
            h2_x = dx_ca * s_h2 + next_x;
            h2_y = dy_ca * s_h2 + next_y;
        }

        g_bez_p2 = {h1_x, h1_y};
        g_bez_p3 = {h2_x, h2_y};

        // --- Build cubic coefficients in X. --------------------------------
        // Bezier cubic B(t) = (1-t)^3 P1 + 3(1-t)^2 t P2 + 3(1-t) t^2 P3 + t^3 P4
        // expanded in t:
        //   B(t) = a t^3 + b t^2 + c t + P1
        // where (decomp aliasing v46..v48):
        //   c = 3*(P2 - P1)
        //   b = 3*(P1 - 2 P2 + P3)
        //   a = P4 - P1 + 3*(P2 - P3)    (decomp: v49 = BezierPt4 + v46,
        //                                 v46 = 3 P2 - P1 - 3 P3)
        const double coef_c_x = 3.0 * h1_x - 3.0 * curr_x;             // v47
        const double coef_b_x = 3.0 * curr_x - 6.0 * h1_x + 3.0 * h2_x; // v48
        const double coef_a_x = next_x + (3.0 * h1_x - curr_x - 3.0 * h2_x); // v49

        // --- Walk integer x from curr_x+1 to next_x. -----------------------
        const int last_x = static_cast<int>(xs[seg + 1]);
        while (x_cursor < last_x) {
            ++x_cursor;
            // Cubic in t: coef_a_x * t^3 + coef_b_x * t^2 + coef_c_x * t + (P1.x - x) = 0
            CubicRoots r = solve_order3(coef_a_x, coef_b_x, coef_c_x,
                                        curr_x - static_cast<double>(x_cursor));

            // Decomp clamps any root > 1.0 back to -1.0 (sentinel).
            double t0 = (r.r0 > 1.0) ? -1.0 : r.r0;
            double t1 = (r.r1 > 1.0) ? -1.0 : r.r1;
            double t2 = (r.r2 > 1.0) ? -1.0 : r.r2;

            if (t0 < 0.0 && t1 < 0.0) {
                // No valid root -> linear fallback between P1 and P4.
                //   out = ( (P4.x - x)/(P4.x-P1.x) * P1.y
                //         + (x - P1.x)/(P4.x-P1.x) * P4.y ) * 4
                const double denom = (next_x - curr_x);
                const double w_lo  = (next_x - static_cast<double>(x_cursor)) / denom;
                const double w_hi  = (static_cast<double>(x_cursor) - curr_x) / denom;
                double v = (w_lo * curr_y + w_hi * next_y) * kYScaleOut;
                out[x_cursor] = v;
                continue;
            }

            // Pick the in-range t preferring positive values (matches the
            // tangle of v52..v56 comparisons in the decomp).
            double t = t0;
            if (t1 > 0.0) t = t1;
            if (t < 0.0) t = t1;
            if (t < 0.0) t = t2;
            if (t2 > 0.0) t = t2;     // last override — matches decomp
            // Finally, if still negative, clamp.
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;

            // Evaluate the Bezier Y in the original factored form:
            //   y = (1-t)^3 P1.y + 3 (1-t)^2 t P2.y + 3 (1-t) t^2 P3.y + t^3 P4.y
            const double one_mt = 1.0 - t;
            double y_fit =
                  one_mt * one_mt * one_mt * curr_y
                + 3.0 * one_mt * one_mt * t   * h1_y
                + 3.0 * one_mt * t    * t     * h2_y
                + t    * t    * t             * next_y;
            y_fit *= kYScaleOut;
            if (y_fit > kMaxY) y_fit = kMaxY;
            if (y_fit < 0.0)   y_fit = 0.0;
            out[x_cursor] = y_fit;
        }

        ++seg;
    }
}

// ---------------------------------------------------------------------------
// CalculateOneGammaCurve @ 0xDF14 — wrapper.
// ---------------------------------------------------------------------------
void calculate_one_gamma_curve(const uint16_t* xs,
                               const uint16_t* ys,
                               int n_points,
                               int16_t* out_1024)
{
    double scratch[1024];
    std::memset(scratch, 0, sizeof(scratch));
    bezier_fit(xs, ys, n_points, scratch);
    for (int i = 0; i < 1024; ++i) {
        int iv = static_cast<int>(scratch[i]);  // decomp uses implicit trunc
        if (iv < 0)      iv = 0;
        if (iv > 4095)   iv = 4095;
        out_1024[i] = static_cast<int16_t>(iv);
    }
}

} // namespace pqgamma
} // namespace hy310
