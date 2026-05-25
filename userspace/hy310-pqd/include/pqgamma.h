/*
 * hy310-pqd — Gamma LUT generator + direct-MMIO writer.
 *
 * Reference: libhaldisplay.so
 *   THal_Vp_SetGamma       @ 0x79F1
 *   CalculateGamma         @ 0xBC49 (4.2 KB NEON SIMD core)
 *   CalculateGammaCurve    @ 0xDDED
 *   CalculateOneGammaCurve @ 0xDF15
 *   WriteGammaLUTByColor   @ 0xB881 (direct MMIO writer — what we mirror)
 *
 * The stock libtvpq HAL writes gamma data directly to display controller
 * MMIO registers — NOT via the MIPS IPC channel.  We reproduce that path
 * so gamma works without needing MIPS hal_adapter_init to complete.
 *
 * Register map (physical addresses, confirmed from libhaldisplay disasm):
 *   0x051BFFA8  (85721320)  Display / TCON control
 *                bit 23 (0x00800000) : LUT write path enable
 *                bit 22 (0x00400000) : color-channel enable latch
 *                bit 30 (0x40000000) : commit trigger
 *   0x051C0034  (85721460)  Display status (high 16 bits = state)
 *   0x052073C0  (86016000)  LUT bulk-write port, 512 × u32
 *                Each u32 packs two 12-bit samples:
 *                  u32[i] = (samples[2*i + 1] << 12) | samples[2*i]
 *                512 dwords × 2 samples = 1024 LUT entries per colour.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace hy310 {
namespace pqgamma {

/* Physical register addresses.
 *
 * Decoded from libhaldisplay.so WriteGammaLUTByColor (file off 0xB881):
 *     j_WriteRegMaskU32(85721320, ...)   → 85721320 = 0x051C00E8
 *     j_ReadRegU32     (85721460)        → 85721460 = 0x051C0174
 *     j_WriteRegBulkU32(86016000, 512, ) → 86016000 = 0x05208000
 *
 * Stock maps these via /dev/hidtvreg, mmap'ing these physical ranges:
 *     0x051C0000 + 64 KB  (REG051CBASE)
 *     0x05200000 +  4 KB  (REG0520BASE)      — NOT enough for 0x05208000
 *     0x05240000 + 64 KB  (REG0524BASE)      — also doesn't contain 0x05208000
 * so there must be yet another mapping we haven't enumerated, or stock
 * expands REG051CBASE to 0x80000 (it asked for 0x10000 = 64 KB).  We
 * map a bigger window via /dev/mem to cover everything we need. */
constexpr uint32_t REG_DISPLAY_CTRL  = 0x051C00E8;
constexpr uint32_t REG_DISPLAY_STAT  = 0x051C0174;

/* Three separate LUT banks, one per colour channel.  Each bank is 2 KB
 * (512 × u32 where each u32 packs two 12-bit samples). */
constexpr uint32_t REG_LUT_CH_R      = 0x05208000;
constexpr uint32_t REG_LUT_CH_G      = 0x05208800;
constexpr uint32_t REG_LUT_CH_B      = 0x05209000;
constexpr uint32_t REG_LUT_BULK      = REG_LUT_CH_R;  // back-compat alias

/* Map from 0x051C0000 through 0x05210000 (320 KB) to cover ctrl/stat/lut. */
constexpr uint32_t MMIO_MAP_BASE     = 0x051C0000;
constexpr uint32_t MMIO_MAP_SIZE     = 0x00050000;

/* LUT dimensions. */
constexpr size_t   LUT_ENTRIES       = 1024;         // samples per colour
constexpr size_t   LUT_BULK_DWORDS   = 512;          // dwords written to HW
constexpr size_t   LUT_SAMPLE_BITS   = 12;
constexpr uint32_t LUT_SAMPLE_MASK   = (1u << LUT_SAMPLE_BITS) - 1;

/* Control bits for REG_DISPLAY_CTRL. */
constexpr uint32_t CTRL_LUT_WRITE_EN = 0x00800000;
constexpr uint32_t CTRL_CHAN_LATCH   = 0x00400000;
constexpr uint32_t CTRL_COMMIT       = 0x40000000;

/* Gamma curve defined by 33 control points, values are 12-bit samples. */
struct GammaCurve {
    std::array<int16_t, 33> points;

    /* Identity (straight ramp): LUT[i] = i scaled to 12-bit. */
    static GammaCurve identity();

    /* Power-law curve: LUT[i] = ((i/1023)^gamma_exp) × 4095. */
    static GammaCurve from_exponent(double gamma_exp);

    /* Canonical 2.2 curve (sRGB-like). */
    static GammaCurve standard_22();
};

/* Per-RGB set of control-point curves. */
struct GammaCurveRGB {
    GammaCurve r, g, b;

    /* Build identical curves for all channels (neutral). */
    static GammaCurveRGB uniform(const GammaCurve& c);
};

/* Interpolate a 33-point curve to a full 1024-entry LUT using
 * piecewise-linear interpolation between adjacent control points. */
std::array<int16_t, LUT_ENTRIES> interpolate(const GammaCurve& c);

class GammaWriter {
public:
    GammaWriter();
    ~GammaWriter();

    int  open_device();        // opens /dev/mem, mmaps display regs
    void close_device();
    bool is_open() const { return mmio_ != nullptr; }

    /* Write RGB gamma LUTs to the display controller.
     * Returns 0 on success, negative errno otherwise. */
    int write_lut(const GammaCurveRGB& curves);

    /* Convenience: write a single curve to all 3 colour channels. */
    int write_uniform(const GammaCurve& c) {
        return write_lut(GammaCurveRGB::uniform(c));
    }

    /* Raw register access — exposed for debugging/tests only. */
    uint32_t read_reg(uint32_t phys_addr) const;
    void     write_reg(uint32_t phys_addr, uint32_t value);
    void     write_reg_mask(uint32_t phys_addr, uint32_t value, uint32_t mask);

private:
    int     fd_   = -1;
    void*   mmio_ = nullptr;
    size_t  mmio_size_ = 0;

    /* Pack 1024 12-bit samples into 512 u32s (WriteGammaLUTByColor layout). */
    static std::array<uint32_t, LUT_BULK_DWORDS>
    pack_lut(const std::array<int16_t, LUT_ENTRIES>& lut);

    /* Bulk write 512 dwords to a target register address. */
    void write_bulk(uint32_t phys_addr,
                    const std::array<uint32_t, LUT_BULK_DWORDS>& data);
};

} // namespace pqgamma
} // namespace hy310
