#pragma once

#include <array>
#include <cstdint>

namespace hy310 {
namespace pqgamma {

struct GammaInput {
    uint8_t ucGammaFactor;       // 0..4 (indexes stock dword_4A50 table)
    uint8_t ucColorTemp;         // 0..8 (selects one of 9 factory_gamma banks; decomp L277 v15 > 8 guard)
    uint8_t ucPictureMode;       // 0..6
    uint8_t ucBlackExtension;    // 0..3
    uint8_t ucDCI;               // 0..3
};

using Lut1024 = std::array<int16_t, 1024>;

struct GammaOutputSet {
    Lut1024 cm_cvbs;    // 0x30030000
    Lut1024 cm_hdmi;    // 0x30030001
    Lut1024 cm_svd;     // 0x30030002
    Lut1024 cm_type_3;  // 0x30030003
    Lut1024 cm_type_4;  // 0x30030004
    Lut1024 cm_type_5;  // 0x30030005
};

/* Pure function — given input state, produce all 6 LUTs.
 * Never touches /dev/mem or hardware. */
GammaOutputSet calculate_gamma(const GammaInput& in,
                               const std::array<int16_t, 33>& control_points);

} // namespace pqgamma
} // namespace hy310
