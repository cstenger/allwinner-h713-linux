/*
 * Probe every known MIPS-side routine in the 34-entry PQ surface.
 *
 * For each: install ARM-side stub, call with minimal (count=1, value=0)
 * params, report: OK / null-return / timeout / other errno, and timing.
 *
 * Goal: confirm the hypothesis that ALL 34 routines are registered on
 * MIPS, not just SetMirrorMode.
 */
#include "cpucomm.h"
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace hy310::cpucomm;
using Clock = std::chrono::steady_clock;

static constexpr const char* kRoutines[] = {
    "THal_Vp_SetBrightness",      "THal_Vp_GetBrightness",
    "THal_Vp_SetContrast",        "THal_Vp_GetContrast",
    "THal_Vp_SetSaturation",      "THal_Vp_GetSaturation",
    "THal_Vp_SetHue",             "THal_Vp_GetHue",
    "THal_Vp_SetSharpness",       "THal_Vp_GetSharpness",
    "THal_Vp_SetBacklightWorkMode",
    "THal_Vp_SetGamma",
    "THal_Vp_SetBlackExtension",  "THal_Vp_GetBlackExtension",
    "THal_Vp_SetDCI",             "THal_Vp_GetDCI",
    "THal_Vp_SetTNR",             "THal_Vp_GetTNR",
    "THal_Vp_SetSNR",             "THal_Vp_GetSNR",
    "THal_Vp_SetPictureMode",     "THal_Vp_GetPictureMode",
    "THal_Vp_SetColorManagement", "THal_Vp_GetColorManagement",
    "THal_Vp_SetVideoRange",      "THal_Vp_GetVideoRange",
    "THal_Vp_SetWhiteBalance",
    "THal_Vp_CvbsSetPedestalMode",
    "THal_Vp_DisableScreenCover", "THal_Vp_EnableScreenCover",
    "THal_Vp_GetSignalInfo",
    "THal_Vp_Wce_SetMirrorMode",
    "THal_Vp_Wce_SetWindow",      "THal_Vp_Wce_GetWindow",
    "THal_Vp_Init",              // extra — the loader
};
static constexpr size_t kN = sizeof(kRoutines) / sizeof(kRoutines[0]);

int main() {
    CpuComm cc;
    if (cc.open_device(false) != 0) {
        std::puts("FAIL: cannot open /dev/cpu_comm");
        return 1;
    }
    std::printf("Probing %zu routines (cpu_id=%u)\n\n", kN, cc.get_cpu_id());

    int ok = 0, null_ret = 0, timeout = 0, other = 0;

    for (size_t i = 0; i < kN; ++i) {
        const char* base = kRoutines[i];
        cc.install_routine(base, /*target_cpu=*/1);

        auto full = format_routine_name(base, /*target_cpu=*/1, 0);
        uint32_t cid = name2id(full);

        uint32_t params[11] = { 1, 0 };
        uint32_t result[11] = { 0 };

        auto t0 = Clock::now();
        int r = cc.call(base, /*target_cpu=*/1, params, result);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - t0).count();

        const char* tag;
        if (r == 0)                { tag = "OK        "; ++ok; }
        else if (r == -ETIME || r == -110) { tag = "TIMEOUT   "; ++timeout; }
        else if (r == -14 && ms < 200) { tag = "null-ret  "; ++null_ret; }
        else                       { tag = "err       "; ++other; }

        std::printf("[%2zu/%2zu] %s  %-40s  cid=0x%08x  %4lldms  r=%3d (%s)\n",
                    i+1, kN, tag, base, cid,
                    static_cast<long long>(ms), r, std::strerror(-r));
    }

    std::printf("\n=== summary ===\n");
    std::printf("  OK          : %d\n", ok);
    std::printf("  null-return : %d\n", null_ret);
    std::printf("  timeout     : %d\n", timeout);
    std::printf("  other err   : %d\n", other);
    std::printf("  total       : %zu\n", kN);
    return 0;
}
