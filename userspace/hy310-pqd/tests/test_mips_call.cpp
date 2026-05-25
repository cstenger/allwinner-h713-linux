/*
 * End-to-end test: talk to MIPS via our transport layer.
 *
 * MIPS currently has ONE registered routine: THal_Vp_Wce_SetMirrorMode_1_000
 * at channel_id 0x30. We install a matching ARM-side stub (so FindRoutine
 * resolves the comp_id and populates msg bytes) then fire a CALL.
 *
 * Pass criteria:
 *   - ioctl returns 0 (not -ETIME 62) within ~500 ms  →  MIPS ACKed
 *   - OR: proves that MIPS drops silently (confirmed known state)
 *
 * Run as root on hy310.
 */
#include "cpucomm.h"
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <chrono>

using namespace hy310::cpucomm;
using Clock = std::chrono::steady_clock;

static void log_ms(const char* tag, Clock::time_point start) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  Clock::now() - start).count();
    std::printf("  [%s] t=%lld ms\n", tag, static_cast<long long>(ms));
}

int main() {
    CpuComm cc;

    std::printf("=== opening /dev/cpu_comm ===\n");
    auto t0 = Clock::now();
    int r = cc.open_device(/*register_helpers=*/false);
    if (r != 0) {
        std::printf("FAIL open_device: %s\n", std::strerror(-r));
        return 1;
    }
    log_ms("open", t0);
    std::printf("  cpu_id = %u\n", cc.get_cpu_id());
    std::printf("  slave_ready = %d\n", cc.slave_ready() ? 1 : 0);

    /* Install routine stub matching MIPS's SetMirrorMode.
     * target_cpu=1 (MIPS) so the hash matches MIPS's _1_000 registration. */
    std::printf("\n=== install THal_Vp_Wce_SetMirrorMode stub ===\n");
    t0 = Clock::now();
    r = cc.install_routine("THal_Vp_Wce_SetMirrorMode", /*target_cpu=*/1);
    if (r != 0) {
        std::printf("FAIL install: %s\n", std::strerror(-r));
        return 2;
    }
    log_ms("install", t0);

    /* Derived comp_id — should match MIPS's "_1_000" hash. */
    uint32_t cid = name2id(format_routine_name(
                      "THal_Vp_Wce_SetMirrorMode", /*target_cpu=*/1, 0));
    std::printf("  comp_id = 0x%08x (CRC32 of '%s')\n",
                cid,
                format_routine_name("THal_Vp_Wce_SetMirrorMode",
                                    /*target_cpu=*/1, 0).c_str());

    /* SetMirrorMode takes one int32 param (mirror mode: 0..3). */
    std::printf("\n=== call SetMirrorMode(0) — no-mirror ===\n");
    uint32_t params[11] = { 1, 0 };     // count=1, mode=0 (HVNoMirror)
    uint32_t result[11] = { 0 };
    t0 = Clock::now();
    r = cc.call("THal_Vp_Wce_SetMirrorMode", /*target_cpu=*/1, params, result);
    log_ms("call", t0);
    if (r == 0) {
        std::printf("  OK — MIPS ACKed\n");
        std::printf("  result[0]=%u (count), result[1]=%u\n", result[0], result[1]);
    } else {
        std::printf("  RET %d (%s)\n", r, std::strerror(-r));
        if (r == -ETIME || r == -62) {
            std::printf("  → MIPS did not ACK in 5s (known baseline)\n");
        }
    }

    return 0;
}
