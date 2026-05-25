/*
 * hy310-pqd — service orchestrator.
 *
 * Wires together the transport, gamma-MMIO, and config-parser components
 * into a cohesive service.  Responsible for:
 *   - loading config from disk
 *   - opening /dev/cpu_comm and /dev/mem
 *   - installing the ARM-side routine stubs for the 34 THal_Vp_* surface
 *   - applying initial settings from config (gamma + stubs for rest)
 *   - exposing runtime control via UNIX-socket commands
 */
#pragma once

#include "cpucomm.h"
#include "pqconfig.h"
#include "pqgamma.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hy310 {
namespace pqservice {

struct Options {
    std::string config_dir  = "/etc/hy310/tvconfig";
    std::string socket_path = "/run/hy310-pqd.sock";
    /* Panel factory-gamma-curve-table: 9 banks × 3 ch × 1024 × u16 = 55296 B.
     * Source: dumped from running Stock at MIPS .bss 0x8B48C2F0 via U-Boot md
     * → USB FAT. If file is absent or wrong size, skip upload (daemon still
     * runs but MIPS uses whatever was in its .bss — usually garbage after
     * warm-reboot). */
    std::string factory_gamma_path = "/etc/hy310/factory_gamma.bin";
    bool        apply_initial    = true;   /* push config to HW at startup */
    bool        install_stubs    = true;   /* register MIPS-side stubs     */
    bool        upload_factory_gamma = true; /* push factory_gamma.bin → MIPS */
    bool        daemon_mode      = false;  /* run forever; false = oneshot */
    /* 2026-04-22 kernel-MM bisection flags */
    bool        skip_lut         = false;  /* --skip-lut: skip gamma write_lut in apply_all */
    bool        skip_mips_calls  = false;  /* --skip-mips-calls: skip 10 mips_call in apply_all */
    int         verbose          = 1;
};

/* Controller values clamped to the Stock slider ranges. Anything outside
 * is rejected. */
struct PQState {
    int brightness       = 50;   /* 0..100 */
    int contrast         = 50;   /* 0..100 */
    int saturation       = 50;   /* 0..100 */
    int hue              = 50;   /* 0..100 */
    int sharpness        = 50;   /* 0..100 */
    int backlight        = 100;  /* 0..100 */
    int colortemperature = 0;    /* 0..3 */
    int gamma            = 3;    /* 0..n (gamma index into curve set) */
    int tnr              = 2;    /* 0..3 */
    int snr              = 1;    /* 0..3 */
    int dci              = 2;    /* 0..3 */
    int blackextension   = 1;    /* 0..3 */
    int dynamic_backlight= 0;    /* 0..1 */
    int picture_mode     = 0;    /* hy310::pqconfig::PictureMode */
    int source_type      = 0;    /* hy310::pqconfig::SourceType  */
};

class PQService {
public:
    explicit PQService(Options opt);
    ~PQService();

    int start();                 /* one-shot: load+apply+return         */
    int run();                   /* blocking event loop (daemon mode)   */
    void stop();

    /* Read the factory-gamma-curve-table from `opts_.factory_gamma_path`
     * (55296 bytes = 9 banks × 3 × 1024 × u16) into factory_gamma_.
     * Does NOT push to MIPS — just caches locally. */
    int load_factory_gamma_from_file();

    /* Upload factory_gamma_ to MIPS via the THal_Vp_Init RPC. MIPS copies
     * params[3]-pointed data into its internal .bss at 0x8B48C2F0.
     * Requires load_factory_gamma_from_file() to have succeeded first. */
    int upload_factory_gamma_to_mips();

    /* Raw access to the cached factory-gamma data (empty until load runs). */
    const std::vector<uint8_t>& factory_gamma() const { return factory_gamma_; }

    /* Extract a single bank/channel as 1024-sample u16 LUT.
     * bank ∈ [0..8], channel ∈ [0..2] (R/G/B). Returns empty if not loaded. */
    std::vector<uint16_t> factory_gamma_bank(int bank, int channel) const;

    /* Apply all current values to the hardware.  Gamma goes to DE2 via
     * direct MMIO; the rest is forwarded as THal_Vp_* calls via MIPS —
     * currently they will time-out silently because MIPS has no handler
     * (blocked until hal_adapter_init completes), but the call path is
     * wired so the moment MIPS unblocks, everything works. */
    int apply_all();

    /* Individual setters — each hits the right pathway. */
    int set_gamma(int factor);                   /* index into curve set, 0..n */
    int set_brightness(int v);
    int set_contrast(int v);
    int set_saturation(int v);
    int set_hue(int v);
    int set_sharpness(int v);
    int set_backlight(int v);
    int set_colortemperature(int v);
    int set_tnr(int v);
    int set_snr(int v);
    int set_dci(int v);
    int set_blackextension(int v);
    int set_picture_mode(int mode);

    const PQState& state() const { return state_; }
    const pqconfig::Config& config() const { return config_; }

private:
    Options                           opts_;
    pqconfig::Config                  config_;
    cpucomm::CpuComm                  transport_;
    pqgamma::GammaWriter              gamma_;
    PQState                           state_;
    std::vector<uint8_t>              factory_gamma_;   /* 55296 B after load */

    /* --- internal helpers --- */

    /* Build a gamma curve from the active picture mode's gamma factor. */
    pqgamma::GammaCurveRGB compose_gamma_curve();

    /* Call a MIPS-side routine.  Returns the errno (negated) from the
     * transport layer — a "normal" outcome right now is ETIME because
     * MIPS cannot dispatch. We log but do not treat as fatal. */
    int mips_call(std::string_view base_name, int param);

    /* Register our stub for a THal_Vp_* so FindRoutine resolves. */
    int install_stub(std::string_view base_name);

    /* Ingest initial values from Config::custom into state_. */
    void seed_state_from_config();

    /* The daemon's UNIX-socket command loop (run in its own thread). */
    int  socket_server();
    void handle_client(int fd);

    bool stop_ = false;
    int  socket_fd_ = -1;
};

/* ---- small command-line tool helpers ---------------------------- */

/* Parse "--key=value" or "--key value" style argv into Options. */
int parse_args(int argc, char** argv, Options& out);

} // namespace pqservice
} // namespace hy310
