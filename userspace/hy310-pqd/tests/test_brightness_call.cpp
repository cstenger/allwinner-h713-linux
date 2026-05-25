// Einzelner SetBrightness-Call — Smoke-Test.
// Beweist dass der komplette Stack (open→install→call) für eine
// echte PQ-Routine durchgeht und MIPS ACKt.
#include "cpucomm.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <cstdlib>

using namespace hy310::cpucomm;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    int value = (argc > 1) ? std::atoi(argv[1]) : 50;  // 0..100 typisch
    CpuComm cc;

    int r = cc.open_device(false);
    if (r != 0) { std::printf("open_device: %s\n", std::strerror(-r)); return 1; }
    std::printf("cpu_id=%u slave_ready=%d\n",
                cc.get_cpu_id(), cc.slave_ready() ? 1 : 0);

    r = cc.install_routine("THal_Vp_SetBrightness", /*target_cpu=*/1);
    if (r != 0) { std::printf("install: %s\n", std::strerror(-r)); return 2; }
    uint32_t cid = name2id(format_routine_name(
                      "THal_Vp_SetBrightness", /*target_cpu=*/1, 0));
    std::printf("comp_id = 0x%08x (expect 0x7221d017)\n", cid);

    // SetBrightness-Signatur (nach Stock libtvpq): (source, value)
    // params[0] = count, danach Args.
    uint32_t params[11] = { 2, /*source=*/0, (uint32_t)value };
    uint32_t result[11] = { 0 };

    auto t0 = Clock::now();
    r = cc.call("THal_Vp_SetBrightness", /*target_cpu=*/1, params, result);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  Clock::now() - t0).count();
    std::printf("call → %d (%s) in %lld ms\n",
                r, r ? std::strerror(-r) : "OK",
                static_cast<long long>(ms));
    if (r == 0) {
        std::printf("result[0..3] = %u %u %u %u\n",
                    result[0], result[1], result[2], result[3]);
    }
    return r == 0 ? 0 : 3;
}
