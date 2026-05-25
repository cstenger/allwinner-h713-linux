/*
 * Trid_Util_Name2ID — CRC32 with seed 0x123456, standard Ethernet polynomial.
 *
 * Source-of-truth: /opt/hy310/stock-re/super_extracted/vendor/lib/libUtility.so
 *   Trid_Util_Name2ID @ 0x1DC61
 *   Trid_Util_CRC32   @ 0x19641
 *   table:              @ 0x0C804 (TriHidtv_crc32_table, 256 × u32)
 *
 * Polynomial confirmed as IEEE 802.3 / Ethernet (reversed 0xEDB88320) by
 * matching entries [0x01]=0x77073096, [0x08]=0x0EDB8832.
 */
#include "cpucomm.h"

#include <array>
#include <cstring>

namespace hy310 {
namespace cpucomm {
namespace {

/* Generate the standard CRC32 table at compile-time so we don't ship a
 * second copy. Identical to stock TriHidtv_crc32_table byte-for-byte. */
constexpr std::array<uint32_t, 256> make_crc32_table() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto CRC_TABLE = make_crc32_table();

} // namespace

uint32_t name2id(std::string_view name) {
    /* Stock returns -1 for empty name; we mirror that as uint32_t max. */
    if (name.empty()) return 0xFFFFFFFFu;

    uint32_t crc = NAME2ID_SEED;
    for (unsigned char b : name) {
        crc = CRC_TABLE[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

std::string format_routine_name(std::string_view base, unsigned cpu_id, unsigned pid) {
    /* Stock: sprintf("%s_%1x_%3.3x", base, cpu_id, pid & 0xFFF) */
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.*s_%1x_%3.3x",
                  static_cast<int>(base.size()), base.data(),
                  cpu_id & 0xF, pid & 0xFFF);
    return std::string(buf);
}

} // namespace cpucomm
} // namespace hy310
