/*
 * Hardware test: access display controller gamma registers via /dev/mem.
 *
 * First phase is READ-ONLY — we print register values to confirm the MMIO
 * region is reachable.  Writing is guarded by argv[1]="--write".
 *
 * Run as root on hy310.
 */
#include "pqgamma.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

using namespace hy310::pqgamma;

int main(int argc, char** argv) {
    bool   do_write  = false;
    double gamma_exp = 1.0;  // default = identity
    bool   do_invert = false;
    bool   do_black  = false;
    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--write") { do_write = true; }
        else if (arg1 == "--gamma" && argc > 2) {
            do_write  = true;
            gamma_exp = std::stod(argv[2]);
        }
        else if (arg1 == "--invert") { do_write = true; do_invert = true; }
        else if (arg1 == "--black")  { do_write = true; do_black  = true; }
    }

    GammaWriter gw;
    int r = gw.open_device();
    if (r != 0) {
        std::printf("FAIL open_device: %s\n", std::strerror(-r));
        return 1;
    }
    std::puts("=== /dev/mem mapped ===");

    /* Phase 1: dump current register state (read-only, safe) */
    std::printf("\n--- Register snapshot ---\n");
    uint32_t ctrl = gw.read_reg(REG_DISPLAY_CTRL);
    uint32_t stat = gw.read_reg(REG_DISPLAY_STAT);
    std::printf("  CTRL  @ 0x%08x = 0x%08x\n", REG_DISPLAY_CTRL, ctrl);
    std::printf("  STAT  @ 0x%08x = 0x%08x  (hi16 = 0x%04x)\n",
                REG_DISPLAY_STAT, stat, stat >> 16);
    std::printf("  LUT   @ 0x%08x = 0x%08x  (bulk port, meaning undef)\n",
                REG_LUT_BULK, gw.read_reg(REG_LUT_BULK));

    /* Interpret CTRL bits */
    std::printf("\n  CTRL bit analysis:\n");
    std::printf("    LUT_WRITE_EN (0x%08x)  %s\n",
                CTRL_LUT_WRITE_EN,
                (ctrl & CTRL_LUT_WRITE_EN) ? "SET" : "clear");
    std::printf("    CHAN_LATCH   (0x%08x)  %s\n",
                CTRL_CHAN_LATCH,
                (ctrl & CTRL_CHAN_LATCH)   ? "SET" : "clear");
    std::printf("    COMMIT       (0x%08x)  %s\n",
                CTRL_COMMIT,
                (ctrl & CTRL_COMMIT)       ? "SET" : "clear");

    /* Phase 2: curve generation (pure computation) */
    std::printf("\n--- Curve samples ---\n");
    for (auto [name, curve] : (const std::pair<const char*, GammaCurve>[]){
             {"identity",     GammaCurve::identity()},
             {"gamma_2_2",    GammaCurve::standard_22()},
             {"gamma_1_8",    GammaCurve::from_exponent(1.8)},
         }) {
        std::printf("  %s:\n    points[0]=%d points[8]=%d points[16]=%d "
                    "points[24]=%d points[32]=%d\n",
                    name, curve.points[0], curve.points[8],
                    curve.points[16], curve.points[24], curve.points[32]);
    }

    /* Phase 3: actually write gamma LUT — DANGEROUS on live display */
    if (!do_write) {
        std::puts("\n(pass --write to actually write identity LUT to hardware)");
        return 0;
    }

    GammaCurve curve = GammaCurve::identity();
    const char* desc = "identity (pass-through)";
    if (do_invert) {
        /* Reverse ramp: 4095 → 0 — negative/inverted image */
        for (size_t i = 0; i < curve.points.size(); ++i)
            curve.points[i] = 4095 - ((i * 4095) / (curve.points.size() - 1));
        desc = "INVERTED (negative image)";
    } else if (do_black) {
        /* All zeros — screen should go BLACK */
        for (auto& p : curve.points) p = 0;
        desc = "ALL-BLACK (screen should turn BLACK)";
    } else if (gamma_exp != 1.0) {
        curve = GammaCurve::from_exponent(gamma_exp);
        desc  = (gamma_exp > 1.0) ? "gamma > 1 (darker mid-tones)"
                                  : "gamma < 1 (brighter mid-tones)";
    }

    std::printf("\n--- WRITING %s ---\n", desc);
    std::printf("  curve sample: [0]=%d [16]=%d [32]=%d\n",
                curve.points[0], curve.points[16], curve.points[32]);

    r = gw.write_uniform(curve);
    if (r != 0) {
        std::printf("FAIL write_lut: %s\n", std::strerror(-r));
        return 2;
    }
    std::puts("  write_lut returned OK");

    /* Dump post-write registers */
    std::printf("\n--- Post-write snapshot ---\n");
    std::printf("  CTRL = 0x%08x (before=0x%08x, delta=0x%08x)\n",
                gw.read_reg(REG_DISPLAY_CTRL), ctrl,
                gw.read_reg(REG_DISPLAY_CTRL) ^ ctrl);
    std::printf("  STAT = 0x%08x (before=0x%08x, delta=0x%08x)\n",
                gw.read_reg(REG_DISPLAY_STAT), stat,
                gw.read_reg(REG_DISPLAY_STAT) ^ stat);
    return 0;
}
