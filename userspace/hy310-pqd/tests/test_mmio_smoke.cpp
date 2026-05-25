/*
 * MMIO smoke test: write a distinctive pattern to CTRL and LUT_BULK,
 * read back to see if the write landed.
 *
 * This tells us whether the display-engine MMIO region is reachable at
 * all or whether it's gated off (clock/reset not enabled).
 */
#include "pqgamma.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>

using namespace hy310::pqgamma;

int main() {
    GammaWriter gw;
    if (gw.open_device() < 0) {
        std::puts("FAIL open_device");
        return 1;
    }

    std::puts("=== CTRL register write smoke ===");
    uint32_t before = gw.read_reg(REG_DISPLAY_CTRL);
    std::printf("  before = 0x%08x\n", before);

    /* Write full-bits, see if anything persists. */
    gw.write_reg(REG_DISPLAY_CTRL, 0xAABBCCDD);
    uint32_t after = gw.read_reg(REG_DISPLAY_CTRL);
    std::printf("  after  = 0x%08x  (delta = 0x%08x)\n",
                after, after ^ before);
    /* Restore */
    gw.write_reg(REG_DISPLAY_CTRL, before);
    std::printf("  restored = 0x%08x\n\n", gw.read_reg(REG_DISPLAY_CTRL));

    std::puts("=== LUT_BULK write smoke ===");
    before = gw.read_reg(REG_LUT_BULK);
    std::printf("  before = 0x%08x\n", before);
    gw.write_reg(REG_LUT_BULK, 0x12345678);
    after = gw.read_reg(REG_LUT_BULK);
    std::printf("  after  = 0x%08x  (delta = 0x%08x)\n",
                after, after ^ before);

    std::puts("\n=== STAT register smoke (read-only expected) ===");
    before = gw.read_reg(REG_DISPLAY_STAT);
    std::printf("  before = 0x%08x\n", before);
    gw.write_reg(REG_DISPLAY_STAT, 0xDEADBEEF);
    after = gw.read_reg(REG_DISPLAY_STAT);
    std::printf("  after  = 0x%08x  (delta = 0x%08x)\n",
                after, after ^ before);

    /* Probe other addresses in the region to see which ones are writable. */
    std::puts("\n=== probe nearby registers ===");
    for (uint32_t off : {0x051BFFA0u, 0x051BFFA4u, 0x051BFFA8u, 0x051BFFACu,
                         0x05200000u, 0x05200100u, 0x05207000u, 0x052073C0u,
                         0x05207400u}) {
        uint32_t b = gw.read_reg(off);
        gw.write_reg(off, 0xCAFEBABE);
        uint32_t a = gw.read_reg(off);
        gw.write_reg(off, b);  // restore
        std::printf("  0x%08x  read=0x%08x  after_write=0x%08x  writable=%s\n",
                    off, b, a, (a != b || a == 0xCAFEBABE) ? "YES" : "no");
    }

    return 0;
}
