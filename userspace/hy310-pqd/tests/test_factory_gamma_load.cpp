/*
 * Verification test: does MIPS's THal_Vp_Init RPC actually populate the
 * 55296-byte factory_gamma_curve_table?
 *
 * If yes: we don't need tvfactorypq.db.
 * If no:  we need to find another source for factory-calibration data.
 */
#include "cpucomm.h"
#include <cstdio>
#include <cstring>

using namespace hy310::cpucomm;

int main() {
    CpuComm cc;
    if (cc.open_device(false) != 0) {
        std::puts("FAIL: open /dev/cpu_comm");
        return 1;
    }
    if (cc.map_sharedmem() != 0) {
        std::puts("FAIL: map_sharedmem");
        return 1;
    }

    constexpr uint32_t kSize = 55296;
    void* buf = cc.shared_malloc(kSize);
    if (!buf) { std::puts("FAIL: shared_malloc"); return 1; }
    std::memset(buf, 0, kSize);
    uint32_t phys = cc.virt_to_phys(buf);
    std::printf("SharedMem allocated: vir=%p phys=0x%08x size=%u\n",
                buf, phys, kSize);

    if (cc.install_routine("THal_Vp_Init", /*target_cpu=*/1) != 0) {
        std::puts("FAIL: install");
        cc.shared_free(buf);
        return 1;
    }

    auto full = format_routine_name("THal_Vp_Init", /*target_cpu=*/1, 0);
    uint32_t cid = name2id(full);
    uint32_t params[11] = { 4, 3, 0, 1, phys };
    uint32_t result[11] = { 0 };

    std::printf("calling %s (cid=0x%08x)...\n", full.c_str(), cid);
    int r = cc.call("THal_Vp_Init", /*target_cpu=*/1, params, result);
    std::printf("return: %d   result[0..3] = %u %u %u %u\n",
                r, result[0], result[1], result[2], result[3]);

    if (r != 0) {
        std::puts("FAIL: MIPS call returned error");
        cc.shared_free(buf);
        return 2;
    }

    /* Inspect the buffer — if MIPS wrote to it, we should see structure. */
    const uint8_t* bytes = static_cast<const uint8_t*>(buf);
    size_t non_zero = 0;
    for (size_t i = 0; i < kSize; ++i) if (bytes[i] != 0) ++non_zero;

    std::printf("buffer content: %zu of %u bytes are non-zero (%.1f%%)\n",
                non_zero, kSize, 100.0 * non_zero / kSize);

    /* Peek at the first 3 banks as u16 arrays (expect ascending gamma curves). */
    const uint16_t* u16 = static_cast<const uint16_t*>(buf);
    for (int bank = 0; bank < 3; ++bank) {
        std::printf("\nbank %d, channel 0 sample [0, 100, 500, 1023]: ", bank);
        std::printf("%u %u %u %u\n",
                    u16[bank * 3072 + 0],
                    u16[bank * 3072 + 100],
                    u16[bank * 3072 + 500],
                    u16[bank * 3072 + 1023]);
    }

    /* Heuristics for "valid-looking gamma": */
    bool looks_like_gamma = false;
    {
        uint16_t first = u16[0];
        uint16_t mid   = u16[500];
        uint16_t last  = u16[1023];
        if (first < mid && mid < last && last > 100)
            looks_like_gamma = true;
    }
    std::printf("\nheuristic: first bank looks like a gamma curve? %s\n",
                looks_like_gamma ? "YES" : "NO");

    cc.shared_free(buf);
    return looks_like_gamma ? 0 : 3;
}
