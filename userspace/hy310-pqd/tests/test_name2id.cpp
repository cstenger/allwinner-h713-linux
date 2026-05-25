/*
 * Unit test: Name2ID must match stock's CRC32 for known routine names.
 * Expected values computed against the extracted stock CRC table and
 * verified to match libUtility.so runtime output.
 */
#include "cpucomm.h"
#include <cassert>
#include <cstdio>

using namespace hy310::cpucomm;

struct TestCase { const char* name; uint32_t expected; };

/* Computed using the stock CRC table extracted from libUtility.so:0xC804.
 * See /tmp/pq_re/crc_table.bin on openclaw for the byte-for-byte dump. */
static constexpr TestCase kCases[] = {
    { "THal_Vp_Wce_SetMirrorMode",          0xB17FB7ABu },
    { "THal_Vp_Wce_SetMirrorMode_1_000",    0x09FFC6EBu },
    { "SetMirrorMode",                      0x609A2CA2u },
    { "ResetNoticeCPU",                     0x892D5F22u },
    { "ShowMM",                             0x287FB988u },
};

int main() {
    int failures = 0;
    for (const auto& t : kCases) {
        uint32_t got = name2id(t.name);
        if (got != t.expected) {
            std::printf("FAIL: name2id(%-38s) = 0x%08x (want 0x%08x)\n",
                        t.name, got, t.expected);
            ++failures;
        } else {
            std::printf(" OK : name2id(%-38s) = 0x%08x\n", t.name, got);
        }
    }

    /* Empty name should return sentinel (stock returns -1). */
    if (name2id("") != 0xFFFFFFFFu) {
        std::puts("FAIL: empty name sentinel");
        ++failures;
    }

    /* Formatting */
    auto s = format_routine_name("THal_Vp_SetGamma", 1, 0);
    if (s != "THal_Vp_SetGamma_1_000") {
        std::printf("FAIL: format → '%s'\n", s.c_str());
        ++failures;
    } else {
        std::printf(" OK : format → '%s'\n", s.c_str());
    }

    return failures == 0 ? 0 : 1;
}
