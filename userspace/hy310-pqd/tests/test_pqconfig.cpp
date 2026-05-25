/*
 * Config parser test: load the real stock files and print a summary.
 */
#include "pqconfig.h"
#include <cstdio>
#include <string>

using namespace hy310::pqconfig;

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "/etc/hy310/tvconfig";

    Config cfg;
    int errs = load_all(dir, cfg);
    std::printf("=== load_all(%s) returned %d missing files ===\n\n", dir.c_str(), errs);

    std::printf("Picture_Mode rows:   %zu\n", cfg.picture_modes.size());
    for (const auto& r : cfg.picture_modes) {
        std::printf("  tvin=%d mode=%d name=%-10s bright=%d contrast=%d gamma=%d colortemp=%d dci=%d be=%d\n",
                    r.tvin, r.mode, r.name.c_str(), r.brightness, r.contrast,
                    r.gamma, r.colortemperature, r.dci, r.blackextension);
    }

    std::printf("\nWhite_Balance rows:  %zu\n", cfg.white_balance.size());
    for (size_t i = 0; i < cfg.white_balance.size() && i < 5; ++i) {
        const auto& r = cfg.white_balance[i];
        std::printf("  tvin=%d mode=%d R(gain=%d,off=%d) G(%d,%d) B(%d,%d)\n",
                    r.tvin, r.mode, r.rgain, r.roffset, r.ggain, r.goffset,
                    r.bgain, r.boffset);
    }

    std::printf("\nGamma_Point (33 values):");
    bool all_zero = true;
    for (size_t i = 0; i < cfg.gamma_points.size(); ++i) {
        if (i % 8 == 0) std::printf("\n  [%02zu]", i);
        std::printf(" %4d", cfg.gamma_points[i]);
        if (cfg.gamma_points[i] != 0) all_zero = false;
    }
    std::printf("\n  (all zero: %s)\n", all_zero ? "yes — Stock defaults uncalibrated" : "no");

    std::printf("\nCustom setting:\n");
    std::printf("  current source type: %d\n", cfg.custom.current_source_type);
    std::printf("  current sliders: brightness=%d contrast=%d gamma=%d colortemp=%d tnr=%d snr=%d dci=%d\n",
                cfg.custom.current_data.brightness,
                cfg.custom.current_data.contrast,
                cfg.custom.current_data.gamma,
                cfg.custom.current_data.colortemperature,
                cfg.custom.current_data.tnr,
                cfg.custom.current_data.snr,
                cfg.custom.current_data.dci);
    std::printf("  modes: videodec=%s hdmi1=%s hdmi2=%s cvbs=%s\n",
                cfg.custom.mode_videodec.c_str(),
                cfg.custom.mode_hdmi1.c_str(),
                cfg.custom.mode_hdmi2.c_str(),
                cfg.custom.mode_cvbs.c_str());

    std::printf("\nColor temp entries by source: %zu sources\n", cfg.color_temp.size());
    for (const auto& [src, entries] : cfg.color_temp) {
        std::printf("  source %d: %zu modes\n", static_cast<int>(src), entries.size());
        for (const auto& e : entries) {
            std::printf("    %-10s gain(%d,%d,%d) offset(%d,%d,%d)\n",
                        e.mode_name.c_str(),
                        e.rgain, e.ggain, e.bgain,
                        e.roffset, e.goffset, e.boffset);
        }
    }

    return 0;
}
