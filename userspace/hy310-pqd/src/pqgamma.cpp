/*
 * Gamma LUT generator + direct-MMIO writer implementation.
 */
#include "pqgamma.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hy310 {
namespace pqgamma {

/* --------------------------------------------------------------- *
 * GammaCurve constructors
 * --------------------------------------------------------------- */

GammaCurve GammaCurve::identity() {
    GammaCurve c{};
    /* 33 evenly-spaced control points from 0 to 4095 (12-bit full range). */
    for (size_t i = 0; i < c.points.size(); ++i) {
        double t = static_cast<double>(i) / (c.points.size() - 1);
        c.points[i] = static_cast<int16_t>(std::lround(t * LUT_SAMPLE_MASK));
    }
    return c;
}

GammaCurve GammaCurve::from_exponent(double gamma_exp) {
    GammaCurve c{};
    for (size_t i = 0; i < c.points.size(); ++i) {
        double t   = static_cast<double>(i) / (c.points.size() - 1);
        double val = std::pow(t, gamma_exp) * LUT_SAMPLE_MASK;
        c.points[i] = static_cast<int16_t>(std::lround(val));
    }
    return c;
}

GammaCurve GammaCurve::standard_22() {
    return from_exponent(2.2);
}

GammaCurveRGB GammaCurveRGB::uniform(const GammaCurve& c) {
    return GammaCurveRGB{c, c, c};
}

/* --------------------------------------------------------------- *
 * Interpolation — 33 control points → 1024 LUT entries
 * --------------------------------------------------------------- */

std::array<int16_t, LUT_ENTRIES> interpolate(const GammaCurve& c) {
    std::array<int16_t, LUT_ENTRIES> lut{};

    /* Distribute 33 points over [0, LUT_ENTRIES-1]. Each segment covers
     * step = (LUT_ENTRIES-1) / (33-1) ≈ 31.96875 LUT entries.
     *
     * We use fixed-point 16.16 to avoid drift: accumulate step in Q16. */
    constexpr int    SEGMENTS = 32;     // 33 points → 32 segments
    const uint32_t   step_q16 = (static_cast<uint32_t>(LUT_ENTRIES - 1) << 16) / SEGMENTS;

    for (int seg = 0; seg < SEGMENTS; ++seg) {
        uint32_t x0_q16 = step_q16 * seg;
        uint32_t x1_q16 = step_q16 * (seg + 1);
        int      x0     = static_cast<int>(x0_q16 >> 16);
        int      x1     = static_cast<int>(x1_q16 >> 16);
        int16_t  y0     = c.points[seg];
        int16_t  y1     = c.points[seg + 1];
        int      dx     = x1 - x0;
        int      dy     = y1 - y0;

        for (int x = x0; x <= x1 && x < static_cast<int>(LUT_ENTRIES); ++x) {
            int frac = (dx > 0) ? ((x - x0) * dy) / dx : 0;
            int v    = y0 + frac;
            if (v < 0) v = 0;
            if (v > static_cast<int>(LUT_SAMPLE_MASK)) v = LUT_SAMPLE_MASK;
            lut[x] = static_cast<int16_t>(v);
        }
    }

    /* Guarantee endpoints exactly. */
    lut[0]               = c.points[0];
    lut[LUT_ENTRIES - 1] = c.points[32];
    return lut;
}

/* --------------------------------------------------------------- *
 * GammaWriter — MMIO lifecycle
 * --------------------------------------------------------------- */

GammaWriter::GammaWriter() = default;

GammaWriter::~GammaWriter() {
    close_device();
}

int GammaWriter::open_device() {
    if (mmio_) return 0;

    fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_ < 0) {
        std::fprintf(stderr, "pqgamma: open /dev/mem failed: %s\n",
                     std::strerror(errno));
        return -errno;
    }

    mmio_size_ = MMIO_MAP_SIZE;
    mmio_ = ::mmap(nullptr, mmio_size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd_, MMIO_MAP_BASE);
    if (mmio_ == MAP_FAILED) {
        std::fprintf(stderr, "pqgamma: mmap(0x%x, 0x%zx) failed: %s\n",
                     MMIO_MAP_BASE, mmio_size_, std::strerror(errno));
        mmio_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return -errno;
    }
    return 0;
}

void GammaWriter::close_device() {
    if (mmio_ && mmio_ != MAP_FAILED) {
        ::munmap(mmio_, mmio_size_);
    }
    mmio_ = nullptr;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

/* --------------------------------------------------------------- *
 * Register access — all via the mapped MMIO window
 * --------------------------------------------------------------- */

uint32_t GammaWriter::read_reg(uint32_t phys_addr) const {
    if (!mmio_ || phys_addr < MMIO_MAP_BASE ||
        phys_addr >= MMIO_MAP_BASE + mmio_size_)
        return 0;
    auto* p = reinterpret_cast<volatile uint32_t*>(
        static_cast<uint8_t*>(mmio_) + (phys_addr - MMIO_MAP_BASE));
    return *p;
}

void GammaWriter::write_reg(uint32_t phys_addr, uint32_t value) {
    if (!mmio_ || phys_addr < MMIO_MAP_BASE ||
        phys_addr >= MMIO_MAP_BASE + mmio_size_)
        return;
    auto* p = reinterpret_cast<volatile uint32_t*>(
        static_cast<uint8_t*>(mmio_) + (phys_addr - MMIO_MAP_BASE));
    *p = value;
}

void GammaWriter::write_reg_mask(uint32_t phys_addr, uint32_t value, uint32_t mask) {
    uint32_t cur = read_reg(phys_addr);
    write_reg(phys_addr, (cur & ~mask) | (value & mask));
}

void GammaWriter::write_bulk(uint32_t phys_addr,
                              const std::array<uint32_t, LUT_BULK_DWORDS>& data)
{
    if (!mmio_) return;
    auto* dst = reinterpret_cast<volatile uint32_t*>(
        static_cast<uint8_t*>(mmio_) + (phys_addr - MMIO_MAP_BASE));
    for (size_t i = 0; i < data.size(); ++i) {
        dst[i] = data[i];
    }
}

/* --------------------------------------------------------------- *
 * LUT packing — 1024 × s16 samples → 512 × u32
 * --------------------------------------------------------------- */

std::array<uint32_t, LUT_BULK_DWORDS>
GammaWriter::pack_lut(const std::array<int16_t, LUT_ENTRIES>& lut)
{
    /* From libhaldisplay.so WriteGammaLUTByColor NEON loop (simplified):
     *   for i in 0..2048 step 16: load 8 pairs, store 4 u32s.
     *   stored u32 = (sample[odd] << 12) | sample[even]
     *
     * With 1024 samples, 2 per u32 → 512 u32s.  Samples are 12-bit. */
    std::array<uint32_t, LUT_BULK_DWORDS> packed{};
    for (size_t i = 0; i < LUT_BULK_DWORDS; ++i) {
        uint32_t even = static_cast<uint32_t>(lut[2 * i])     & LUT_SAMPLE_MASK;
        uint32_t odd  = static_cast<uint32_t>(lut[2 * i + 1]) & LUT_SAMPLE_MASK;
        packed[i] = (odd << LUT_SAMPLE_BITS) | even;
    }
    return packed;
}

/* --------------------------------------------------------------- *
 * write_lut — the sequence ported from WriteGammaLUTByColor
 * --------------------------------------------------------------- */

int GammaWriter::write_lut(const GammaCurveRGB& curves) {
    if (!mmio_) return -ENODEV;

    /* Exact sequence matches libhaldisplay.so::WriteGammaLUTByColor @ 0xB881.
     *
     * Pre: spin until status-high stabilises (dword_11CDC cache pattern).
     * Arm: LUT_WRITE_EN, CHAN_LATCH, COMMIT on REG_DISPLAY_CTRL.
     * Write: R/G/B each get their own bulk port (2KB apart).
     * Post: 5 activation steps that actually LATCH the new LUT into the
     *       scanout path — without these, the LUT is never applied. */

    /* --- Pre: wait for display to reach a stable frame boundary --- */
    static uint32_t last_stat_hi = 0;
    {
        uint32_t hi = read_reg(REG_DISPLAY_STAT) & 0xFFFF0000u;
        for (int i = 0; i < 40 && hi == last_stat_hi; ++i) {
            usleep(1000);
            hi = read_reg(REG_DISPLAY_STAT) & 0xFFFF0000u;
        }
        last_stat_hi = hi;
    }

    /* Save original CTRL — stock uses the prior bit-28 state in the
     * post-write flip. */
    uint32_t orig_ctrl = read_reg(REG_DISPLAY_CTRL);

    /* --- Arm: enable LUT write path + latch + commit --- */
    write_reg_mask(REG_DISPLAY_CTRL, CTRL_LUT_WRITE_EN, CTRL_LUT_WRITE_EN);
    write_reg_mask(REG_DISPLAY_CTRL, CTRL_CHAN_LATCH,   CTRL_CHAN_LATCH);
    write_reg_mask(REG_DISPLAY_CTRL, CTRL_COMMIT,       CTRL_COMMIT);

    /* --- Write: three separate 2KB LUT banks --- */
    auto lut_r = interpolate(curves.r);
    auto lut_g = interpolate(curves.g);
    auto lut_b = interpolate(curves.b);

    write_bulk(REG_LUT_CH_R, pack_lut(lut_r));
    write_bulk(REG_LUT_CH_G, pack_lut(lut_g));
    write_bulk(REG_LUT_CH_B, pack_lut(lut_b));

    /* --- Post: 5 activation steps (mirror libhaldisplay LABEL_20 tail) ---
     *
     *   1.  clear COMMIT (bit 30)        → release commit gate
     *   2.  flip bit 28                   → double-buffer flip; value is
     *                                       orig_ctrl ^ 0x10000000
     *   3.  set bit 21 (0x200000)         → apply latch A
     *   4.  set bit 20 (0x100000)         → apply latch B
     *   5.  set bit 26 (0x4000000)        → kick scanout to re-sample LUT
     */
    /* Our write_reg_mask(addr, value, mask) semantics is:
     *   reg = (reg & ~mask) | (value & mask)
     * To map stock's WriteRegMaskU32(addr, mask, val), call as (val, mask). */
    write_reg_mask(REG_DISPLAY_CTRL, 0x00000000u,              0x40000000u); // clear COMMIT
    write_reg_mask(REG_DISPLAY_CTRL, orig_ctrl ^ 0x10000000u,  0x10000000u); // flip bit 28
    write_reg_mask(REG_DISPLAY_CTRL, 0x00200000u,              0x00200000u); // set bit 21
    write_reg_mask(REG_DISPLAY_CTRL, 0x00100000u,              0x00100000u); // set bit 20
    write_reg_mask(REG_DISPLAY_CTRL, 0x04000000u,              0x04000000u); // set bit 26

    /* Refresh stat cache so next call waits correctly. */
    last_stat_hi = read_reg(REG_DISPLAY_STAT) & 0xFFFF0000u;
    return 0;
}

} // namespace pqgamma
} // namespace hy310
