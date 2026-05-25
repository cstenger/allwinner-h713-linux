/*
 * Diagnostic: try to call the MIPS-side THal_Vp_Init RPC.
 *
 * Hypothesis check: if MIPS has THal_Vp_Init registered, the call
 * returns cleanly in ~30 ms (like SetMirrorMode). If MIPS has it BUT
 * hal_adapter_init is blocked, we'll see our existing "null return"
 * pattern. If MIPS has NOT registered it at all, we'll see a 5 s
 * timeout (the current behaviour for any routine other than
 * SetMirrorMode_1_000).
 *
 * We do NOT allocate a real shared-memory buffer for the phys-addr
 * param — pass 0 and observe MIPS's reaction.  Even a call with a
 * bogus pointer should return SOMETHING if the routine is registered
 * (probably an error code in result[]), which is all we need to learn.
 */
#include "cpucomm.h"
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace hy310::cpucomm;
using Clock = std::chrono::steady_clock;

static void probe_one(CpuComm& cc, const char* base_name, int extra_param)
{
    int r = cc.install_routine(base_name, /*target_cpu=*/1);
    if (r != 0) {
        std::printf("  install  : FAILED (%s)\n", std::strerror(-r));
        return;
    }

    auto name_full = format_routine_name(base_name, /*target_cpu=*/1, 0);
    uint32_t cid   = name2id(name_full);
    std::printf("  comp_id  : 0x%08x  (%s)\n", cid, name_full.c_str());

    uint32_t params[11] = {
        1,                              // count
        static_cast<uint32_t>(extra_param)
    };
    uint32_t result[11] = {0};

    auto t0 = Clock::now();
    int call_ret = cc.call(base_name, /*target_cpu=*/1, params, result);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - t0).count();

    std::printf("  call     : ret=%d (%s), %lld ms\n",
                call_ret, std::strerror(-call_ret),
                static_cast<long long>(ms));
    std::printf("  result[0..3]: %u %u %u %u\n",
                result[0], result[1], result[2], result[3]);

    if (call_ret == 0)
        std::puts("  ⇒ MIPS SIDE HAS THIS ROUTINE REGISTERED ✓");
    else if (ms < 200)
        std::puts("  ⇒ MIPS reacted fast but with error (routine exists, call invalid)");
    else if (ms < 1000)
        std::puts("  ⇒ kernel-local rejection");
    else
        std::puts("  ⇒ timeout: MIPS dropped silently (routine not registered)");
}

int main() {
    CpuComm cc;
    if (cc.open_device(false) != 0) {
        std::puts("FAIL: cannot open /dev/cpu_comm");
        return 1;
    }
    std::printf("cpu_id=%u  slave_ready=%d\n\n",
                cc.get_cpu_id(), cc.slave_ready() ? 1 : 0);

    /* Probe #1: known-good baseline */
    std::puts("=== baseline: SetMirrorMode (known to work) ===");
    probe_one(cc, "THal_Vp_Wce_SetMirrorMode", 0);

    /* Probe #2: THal_Vp_Init (our hypothesis target) */
    std::puts("\n=== target: THal_Vp_Init ===");
    probe_one(cc, "THal_Vp_Init", 0);

    /* Probe #3: a random other PQ routine, for comparison */
    std::puts("\n=== comparison: THal_Vp_SetBrightness ===");
    probe_one(cc, "THal_Vp_SetBrightness", 50);

    return 0;
}
