/*
 * hy310-pqd — pq_bezier.h
 *
 * 1:1 C++ port of the four math routines from libhaldisplay.so:
 *     BezierFit             @ 0xD851
 *     Solve_Order3_Equation @ 0xD089
 *     Solve_Order2_Equation @ 0xCF91
 *     Newton_Solve          @ 0xCE71
 *
 * Evidence and provenance: /opt/hy310/gamma_re/solvers_decomp.txt,
 * /opt/hy310/gamma_re/BEZIER_REPORT.md.
 *
 * IMPORTANT — re: the "BezierPt1..4 constant tables":
 *   readelf places BezierPt1 @ 0x20CF0 .. BezierPt4 @ 0x20D20 (16 B each)
 *   in the .bss section (section 22, .bss starts at VA 0x11A90). The
 *   decompilation shows these are *runtime working variables* written and
 *   read inside BezierFit itself, NOT constant template tables. Each 16-byte
 *   slot holds 2 doubles = {x, y} of one control point of the current
 *   sub-Bezier. We therefore expose them as file-scope `thread_local`
 *   working storage mirroring the stock global layout, not as `const`.
 */

#pragma once

#include <array>
#include <cstdint>

namespace hy310 {
namespace pqgamma {

// ---------------------------------------------------------------------------
// Cubic / quadratic solver result — mirrors the 25-byte struct written at
// `a1` in Solve_Order3_Equation. The three doubles are the three roots;
// `count` is the byte at offset 24 ({0, 2, or -} in stock; see decomp).
// ---------------------------------------------------------------------------
struct CubicRoots {
    double r0;     // primary root (written at offset  0)
    double r1;     // secondary root (offset  8)
    double r2;     // tertiary root (offset 16)
    uint8_t count; // count/tag byte (offset 24). 2 = at least two real roots.
};

// Quadratic — same layout (r2 unused).
using QuadRoots = CubicRoots;

// ---------------------------------------------------------------------------
// Solve a·x² + b·x + c = 0 (NB: stock names `a3, a4` for x and const terms).
// Port of Solve_Order2_Equation @ 0xCF91.
//
// Returns a filled CubicRoots where:
//   r0, r1 = the two real roots (r0 <= r1, sorted as in stock),
//   r2     = sentinel -1.0 (stock writes 0xBFF00000 high / 0 low = -1.0),
//   count  = 2 if real roots exist, 0 if discriminant < 0 and no linear
//            degenerate case applied.
// ---------------------------------------------------------------------------
QuadRoots solve_order2(double a, double b, double c);

// ---------------------------------------------------------------------------
// Solve a·x³ + b·x² + c·x + d = 0 (stock param names: a2=a, a3=b, a4=c, a5=d).
// Port of Solve_Order3_Equation @ 0xD089.
//
// Uses a globally-scoped polynomial-coefficient copy (a3, a2, a1, a0) per
// stock (see the `::a3 = a2; ::a2 = a3; ...` block) to share with the
// Newton_Solve helper. For the port we keep these thread-local.
//
// Returns up to three real roots; `count` is the stock tag byte.
// ---------------------------------------------------------------------------
CubicRoots solve_order3(double a, double b, double c, double d);

// ---------------------------------------------------------------------------
// Newton iteration on the globally-stored cubic (coeffs a3, a2, a1, a0),
// starting from `x0`. Port of Newton_Solve @ 0xCE71.
//
// Iteration bound: `i >> 2 <= 0x7C` => `i <= 499` => up to 500 iterations.
// Convergence tolerance: fabs(f(x)) <= 0.00001 (1e-5).
// Returns the converged x, or -1.0 on failure / singular derivative.
// ---------------------------------------------------------------------------
double newton_solve(double x0);

// ---------------------------------------------------------------------------
// Exact port of BezierFit @ 0xD851.
//
// Stock prototype (from decomp):
//   BezierFit(uint16_t *xs, uint16_t *ys, int n, double *out_1024)
//
// The output buffer MUST be 1024 doubles. Entries are filled in the index
// range [xs[0] .. xs[n-1]]; untouched entries keep their caller value
// (CalculateOneGammaCurve zeros the whole buffer before calling).
// ---------------------------------------------------------------------------
void bezier_fit(const uint16_t* xs,
                const uint16_t* ys,
                int n_points,
                double* out /* [1024] */);

// Convenience wrapper mirroring CalculateOneGammaCurve @ 0xDF14: runs
// bezier_fit into a local 1024-double scratch, truncates each to uint16_t,
// clamps to [0, 4095] implicitly via the stock `* 4.0` + `> 4095 ? 4095`
// already inside bezier_fit.
void calculate_one_gamma_curve(const uint16_t* xs,
                               const uint16_t* ys,
                               int n_points,
                               int16_t* out_1024);

} // namespace pqgamma
} // namespace hy310
