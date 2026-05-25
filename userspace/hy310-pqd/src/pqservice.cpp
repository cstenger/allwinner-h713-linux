/*
 * PQService implementation.
 */
#include "pqservice.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace hy310 {
namespace pqservice {

/* Full list of all 81 MIPS-side HAL routines registered by MIPS's
 * hal_adapter_init (display.bin sub_8B10AD78). Every name here gets an
 * ARM-side proxy stub via install_routine(name, target_cpu=1) so
 * FindRoutine resolves userspace comp_ids and dispatches to MIPS.
 *
 * Source-of-truth: cpu_comm_dev.c:1578+ (mirror of MIPS's registration).
 * Note the lowercase "Thal_" prefix on 3 entries — matches MIPS firmware
 * verbatim (casing must match for CRC32 hash to resolve). */
static constexpr const char* kMipsRoutines[] = {
    /* Lifecycle / pipeline control */
    "THal_Vp_Init",
    "THal_Vp_Deinit",
    "THal_Vp_EnableBlackScreen",
    "THal_Vp_DisableBlackScreen",
    "THal_Vp_EnableVideoFreeze",
    "THal_Vp_DisableVideoFreeze",
    "THal_Vp_EnableScreenCover",
    "THal_Vp_DisableScreenCover",
    "THal_Vp_SetSource",
    "THal_Vp_GetSource",
    /* WCE (window compositor engine) */
    "THal_Vp_Wce_SetMirrorMode",
    "THal_Vp_Wce_SetWindow",
    "THal_Vp_Wce_GetWindow",
    "THal_Vp_Wce_GetActiveWindow",
    "THal_Vp_Wce_EnablePixel2PixelMode",
    "THal_Vp_Wce_DisablePixel2PixelMode",
    "THal_Vp_SeamlessEnable",
    "THal_Vp_SeamlessDisable",
    /* PQ sliders (picture quality) */
    "THal_Vp_SetBrightness",     "THal_Vp_GetBrightness",
    "THal_Vp_SetContrast",       "THal_Vp_GetContrast",
    "THal_Vp_SetSaturation",     "THal_Vp_GetSaturation",
    "THal_Vp_SetHue",            "THal_Vp_GetHue",
    "THal_Vp_SetSharpness",      "THal_Vp_GetSharpness",
    "THal_Vp_SetTNR",            "THal_Vp_GetTNR",
    "THal_Vp_SetSNR",            "THal_Vp_GetSNR",
    "THal_Vp_SetDCI",            "THal_Vp_GetDCI",
    "THal_Vp_SetBlackExtension", "THal_Vp_GetBlackExtension",
    "THal_Vp_SetPictureMode",    "THal_Vp_GetPictureMode",
    "THal_Vp_SetColorManagement","THal_Vp_GetColorManagement",
    "THal_Vp_SetLowLatencyMode", "THal_Vp_GetLowLatencyMode",
    "THal_Vp_GetDisplayLatency",
    "THal_Vp_RegisterCallbackOfDisplayLatencyChange",
    "THal_Vp_UnregisterCallbackOfDisplayLatencyChange",
    "THal_Vp_SetWhiteBalance",
    "THal_Vp_SetGamma",
    "THal_Vp_SetVideoRange",     "THal_Vp_GetVideoRange",
    /* Backlight */
    "THal_Vp_SetBacklightWorkMode",
    "Thal_Vp_SetBacklightPwmInfo",   /* lowercase h intentional */
    "Thal_Vp_SetBacklightLevel",     /* lowercase h intentional */
    /* Image buffer */
    "THal_Vp_SetImageBufferAddr",
    "THal_Vp_GetImageBufferAddr",
    /* HDMI / HDCP */
    "THal_Vp_SetHDCP22Key",
    "THal_Vp_TurnOnARCAudioPath",
    "THal_Vp_SwitchARCTXPath",
    "THal_Vp_HDMI_GetPortStatus",
    "THal_Vp_HDMI_SetPortMap",
    "THal_Vp_HDMI_ReloadHdcp14Key",
    "THal_Vp_HDMI_SetHPDTimeInterval",
    "THal_Vp_SetHDMIHotPlugByPortCallback",
    "THal_Vp_UnregisterSignalChangeCallback",
    /* ATV */
    "THal_Vp_AtvChannelScanStart",
    "THal_Vp_AtvChannelScanEnd",
    "THal_Vp_AtvChannelChange",
    "THal_Vp_AtvSetSignalStd",
    "THal_Vp_AtvSetRegion",
    "THal_Vp_AtvEnableSnowScreen",
    "Thal_Vp_AtvIsFastSyncLock",     /* lowercase h intentional */
    /* CVBS / signal info */
    "THal_Vp_CvbsSetPedestalMode",
    "THal_Vp_GetVBIData",
    "THal_Vp_GetSignalInfo",
    "THal_Vp_RegisterSignalChangeCallback",
    /* VBI */
    "THal_Vp_ResetVBI",
    "THal_Vp_EnableVBILine",
    "THal_Vp_GetVBIAddr",
    "THal_Vp_GetVBISize",
    "THal_Vp_GetVBIOffset",
    "THal_Vp_StopVBI",
    "THal_Vp_StartVBI",
};
static constexpr size_t kMipsRoutineCount =
    sizeof(kMipsRoutines) / sizeof(kMipsRoutines[0]);

/* --------------------------------------------------------------- */

PQService::PQService(Options opt) : opts_(std::move(opt)) {}

PQService::~PQService() { stop(); }

void PQService::stop() {
    stop_ = true;
    if (socket_fd_ >= 0) { ::close(socket_fd_); socket_fd_ = -1; }
}

/* --------------------------------------------------------------- *
 * start — one-shot boot sequence.
 * --------------------------------------------------------------- */

int PQService::start() {
    auto log = [&](int lvl, const char* fmt, auto&&... args) {
        if (opts_.verbose >= lvl)
            std::fprintf(stderr, fmt, args...);
    };

    /* 1) Load config from disk. Missing pieces default to safe values. */
    int missing = pqconfig::load_all(opts_.config_dir, config_);
    log(1, "hy310-pqd: config loaded (%d missing files)\n", missing);
    seed_state_from_config();

    /* 2) Open transport to MIPS. */
    if (transport_.open_device(/*register_helpers=*/true) != 0) {
        log(0, "hy310-pqd: CANNOT open /dev/cpu_comm — continuing without MIPS\n");
    } else {
        log(1, "hy310-pqd: /dev/cpu_comm open, cpu_id=%u slave_ready=%d\n",
            transport_.get_cpu_id(), transport_.slave_ready() ? 1 : 0);
    }

    /* 3) Open DE2 MMIO for direct gamma writes. */
    if (gamma_.open_device() != 0) {
        log(0, "hy310-pqd: CANNOT open /dev/mem — gamma path disabled\n");
    } else {
        log(1, "hy310-pqd: DE2 MMIO mapped (0x%08x, %u KB)\n",
            pqgamma::MMIO_MAP_BASE, pqgamma::MMIO_MAP_SIZE / 1024);
    }

    /* 4) Install ARM-side stubs for every MIPS-side routine we'll call.
     * target_cpu=1 (MIPS) so the hash matches MIPS's hal_adapter_init
     * "_1_000" registrations. */
    if (opts_.install_stubs && transport_.is_open()) {
        int ok = 0, fail = 0;
        for (const char* name : kMipsRoutines) {
            if (transport_.install_routine(name, /*target_cpu=*/1) == 0)
                ++ok;
            else
                ++fail;
        }
        log(1, "hy310-pqd: installed %d/%zu routine stubs (%d failed)\n",
            ok, kMipsRoutineCount, fail);
    }

    /* 5) Factory-gamma pipeline: read bin file from disk (dumped from
     *    Stock's MIPS .bss via U-Boot) and upload to MIPS via THal_Vp_Init.
     *    MIPS copies it into its internal table at 0x8B48C2F0 which is
     *    then used as the base for all gamma pipeline work.
     *    Non-fatal if either step fails — daemon proceeds without it. */
    int fg = load_factory_gamma_from_file();
    if (fg == 0) {
        log(1, "hy310-pqd: factory-gamma loaded from %s (%zu B)\n",
            opts_.factory_gamma_path.c_str(), factory_gamma_.size());

        if (opts_.upload_factory_gamma
            && transport_.is_open() && opts_.install_stubs) {
            int up = upload_factory_gamma_to_mips();
            if (up == 0)
                log(1, "hy310-pqd: factory-gamma uploaded to MIPS (%zu B)\n",
                    factory_gamma_.size());
            else
                log(1, "hy310-pqd: factory-gamma upload skipped (%s)\n",
                    std::strerror(-up));
        }
    } else {
        log(1, "hy310-pqd: factory-gamma file not loaded (%s: %s)\n",
            opts_.factory_gamma_path.c_str(), std::strerror(-fg));
    }

    /* 6) Apply initial PQ state. */
    if (opts_.apply_initial)
        apply_all();

    return 0;
}

/* --------------------------------------------------------------- *
 * Factory-gamma table handling.
 *
 * MIPS-side ground truth (RE'd via capstone, 2026-04-18):
 *   THal_Vp_Init @ VA 0x8B109F04 copies FROM user buffer INTO its
 *   internal .bss at 0x8B48C2F0, NOT the other way around.
 *   → It is a SETTER, not a getter.
 *
 * So the flow is:
 *   1. Read factory_gamma.bin (55296 B) from disk. The file was dumped
 *      from a running Stock system via U-Boot md.l/fatwrite.
 *   2. Copy into a shared-memory buffer.
 *   3. Call THal_Vp_Init with params[3] = phys(buf). MIPS then internalises
 *      the data and uses it as the base LUT for all subsequent pipeline
 *      operations (CalculateGamma, BezierFit, etc.).
 *
 * Layout of the 55296 bytes:
 *   9 banks × 3 channels × 1024 samples × u16 LE (12-bit LUT packed into u16)
 *   — only banks 0..2 are populated in the stock dump (Identity-like curve
 *     0..4092); banks 3..8 are zero and appear to be reserved slots.
 * --------------------------------------------------------------- */

namespace {
constexpr uint32_t kFactoryGammaSize = 55296;  // 9 × 3 × 1024 × 2
}

int PQService::load_factory_gamma_from_file() {
    const char* path = opts_.factory_gamma_path.c_str();
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return -ENOENT;

    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz != static_cast<long>(kFactoryGammaSize)) {
        std::fclose(fp);
        return -EINVAL;  /* bad size */
    }

    factory_gamma_.assign(kFactoryGammaSize, 0);
    size_t got = std::fread(factory_gamma_.data(), 1, kFactoryGammaSize, fp);
    std::fclose(fp);
    if (got != kFactoryGammaSize) {
        factory_gamma_.clear();
        return -EIO;
    }
    return 0;
}

int PQService::upload_factory_gamma_to_mips() {
    if (!transport_.is_open())          return -ENODEV;
    if (factory_gamma_.size() != kFactoryGammaSize) return -EINVAL;

    /* IOCTL_MALLOC (0xC0087F10) crashes the kernel's __shmalloc at sizes
     * ≥ ~32 KB (free-list corruption). Bypass by mmap'ing /dev/mem at a
     * fixed high offset inside the 5-MB shared-memory region, which is
     * unused by the cpu_comm subsystem. Page-aligned, no kernel alloc. */
    constexpr uint32_t kShmemPhysBase = cpucomm::SHMEM_PHYS_BASE; /* 0x4E300000 */
    constexpr uint32_t kStagingOffset = 0x00400000;               /* +4 MB */
    constexpr uint32_t kStagingPhys   = kShmemPhysBase + kStagingOffset;

    int fd = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -errno;

    void* map = ::mmap(nullptr, kFactoryGammaSize,
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, kStagingPhys);
    ::close(fd);
    if (map == MAP_FAILED) return -EIO;

    /* Copy file contents INTO the shared-memory staging region. */
    std::memcpy(map, factory_gamma_.data(), kFactoryGammaSize);
    /* Flush to DDR before MIPS reads it. */
    __sync_synchronize();

    transport_.install_routine("THal_Vp_Init", /*target_cpu=*/1);

    uint32_t params[11] = { 4, 3, 0, 1, kStagingPhys };
    uint32_t result[11] = { 0 };
    int ret = transport_.call("THal_Vp_Init", /*target_cpu=*/1, params, result);

    ::munmap(map, kFactoryGammaSize);
    return ret;
}

std::vector<uint16_t>
PQService::factory_gamma_bank(int bank, int channel) const {
    std::vector<uint16_t> out;
    if (factory_gamma_.size() != kFactoryGammaSize) return out;
    if (bank < 0 || bank > 8 || channel < 0 || channel > 2) return out;

    constexpr size_t kBankStride    = 3 * 1024 * 2;  /* 6144 */
    constexpr size_t kChannelStride = 1024 * 2;      /* 2048 */
    const uint8_t* p =
        factory_gamma_.data() + bank * kBankStride + channel * kChannelStride;

    out.resize(1024);
    std::memcpy(out.data(), p, kChannelStride);
    return out;
}

/* --------------------------------------------------------------- *
 * seed_state_from_config
 * --------------------------------------------------------------- */

void PQService::seed_state_from_config() {
    const auto& cs = config_.custom.current_data;
    state_.brightness        = cs.brightness;
    state_.contrast          = cs.contrast;
    state_.saturation        = cs.saturation;
    state_.hue               = cs.hue;
    state_.sharpness         = cs.sharpness;
    state_.backlight         = cs.backlight;
    state_.colortemperature  = cs.colortemperature;
    state_.gamma             = cs.gamma;
    state_.tnr               = cs.tnr;
    state_.snr               = cs.snr;
    state_.dci               = cs.dci;
    state_.blackextension    = cs.blackextension;
    state_.dynamic_backlight = cs.dynamic_backlight;
    state_.source_type       = config_.custom.current_source_type;
    /* picture_mode not in XML directly — derive from the mode_{source} */
    state_.picture_mode      = 0;  /* "standard" default */
}

/* --------------------------------------------------------------- *
 * compose_gamma_curve — build a curve from the current PQ state.
 *
 * Today we use the gamma-factor slider (0..10 typically, default 3) to
 * select a power-law exponent.  Later we'll replace this with stock's
 * CalculateGamma reverse-ported algorithm.
 * --------------------------------------------------------------- */

pqgamma::GammaCurveRGB PQService::compose_gamma_curve() {
    /* Gamma index 3 in Stock = default 2.2 curve.  We map indexes roughly:
     *   0 → 1.6 (bright)
     *   1 → 1.8
     *   2 → 2.0
     *   3 → 2.2 (default)
     *   4 → 2.4
     *   5 → 2.6 (dark) */
    static constexpr double kTable[] = {1.6, 1.8, 2.0, 2.2, 2.4, 2.6, 2.8, 3.0, 3.2, 3.4, 3.6};
    int idx = std::clamp(state_.gamma, 0,
                         static_cast<int>(sizeof(kTable)/sizeof(kTable[0])) - 1);
    auto curve = pqgamma::GammaCurve::from_exponent(kTable[idx]);
    return pqgamma::GammaCurveRGB::uniform(curve);
}

/* --------------------------------------------------------------- *
 * apply_all — push every setting to the hardware.
 * --------------------------------------------------------------- */

int PQService::apply_all() {
    int errors = 0;

    /* Gamma via direct MMIO (works today without MIPS). */
    if (gamma_.is_open() && !opts_.skip_lut) {
        if (gamma_.write_lut(compose_gamma_curve()) != 0)
            ++errors;
    }
    if (opts_.skip_lut)
        std::fprintf(stderr, "hy310-pqd: apply_all: skip_lut — gamma write_lut SKIPPED\n");

    /* Rest via MIPS — these silently fail until hal_adapter_init unblocks. */
    if (transport_.is_open() && !opts_.skip_mips_calls) {
        mips_call("THal_Vp_SetBrightness",     state_.brightness);
        mips_call("THal_Vp_SetContrast",       state_.contrast);
        mips_call("THal_Vp_SetSaturation",     state_.saturation);
        mips_call("THal_Vp_SetHue",            state_.hue);
        mips_call("THal_Vp_SetSharpness",      state_.sharpness);
        mips_call("THal_Vp_SetBacklightWorkMode", state_.backlight);
        mips_call("THal_Vp_SetTNR",            state_.tnr);
        mips_call("THal_Vp_SetSNR",            state_.snr);
        mips_call("THal_Vp_SetDCI",            state_.dci);
        mips_call("THal_Vp_SetBlackExtension", state_.blackextension);
    }
    if (opts_.skip_mips_calls)
        std::fprintf(stderr, "hy310-pqd: apply_all: skip_mips_calls — 10 mips_call SKIPPED\n");
    return errors;
}

/* --------------------------------------------------------------- *
 * individual setters
 * --------------------------------------------------------------- */

int PQService::set_gamma(int factor) {
    state_.gamma = std::clamp(factor, 0, 10);
    if (!gamma_.is_open()) return -ENODEV;
    return gamma_.write_lut(compose_gamma_curve());
}

#define SETTER_MIPS(Name, Field, Lo, Hi, Routine)                       \
int PQService::set_##Field(int v) {                                     \
    state_.Field = std::clamp(v, Lo, Hi);                               \
    return mips_call(Routine, state_.Field);                            \
}

SETTER_MIPS(Brightness,     brightness,       0, 100, "THal_Vp_SetBrightness")
SETTER_MIPS(Contrast,       contrast,         0, 100, "THal_Vp_SetContrast")
SETTER_MIPS(Saturation,     saturation,       0, 100, "THal_Vp_SetSaturation")
SETTER_MIPS(Hue,            hue,              0, 100, "THal_Vp_SetHue")
SETTER_MIPS(Sharpness,      sharpness,        0, 100, "THal_Vp_SetSharpness")
SETTER_MIPS(Backlight,      backlight,        0, 100, "THal_Vp_SetBacklightWorkMode")
SETTER_MIPS(Colortemperature, colortemperature, 0, 3, "THal_Vp_SetWhiteBalance")
SETTER_MIPS(TNR,            tnr,              0,   3, "THal_Vp_SetTNR")
SETTER_MIPS(SNR,            snr,              0,   3, "THal_Vp_SetSNR")
SETTER_MIPS(DCI,            dci,              0,   3, "THal_Vp_SetDCI")
SETTER_MIPS(BlackExtension, blackextension,   0,   3, "THal_Vp_SetBlackExtension")

#undef SETTER_MIPS

int PQService::set_picture_mode(int mode) {
    state_.picture_mode = std::clamp(mode, 0, 6);
    /* Load full PQ preset from config for this mode. */
    auto row = config_.find_mode(state_.source_type, state_.picture_mode);
    if (row) {
        state_.brightness      = row->brightness;
        state_.contrast        = row->contrast;
        state_.saturation      = row->saturation;
        state_.hue             = row->hue;
        state_.sharpness       = row->sharpness;
        state_.backlight       = row->backlight;
        state_.colortemperature= row->colortemperature;
        state_.gamma           = row->gamma;
        state_.tnr             = row->tnr;
        state_.snr             = row->snr;
        state_.dci             = row->dci;
        state_.blackextension  = row->blackextension;
    }
    return apply_all();
}

/* --------------------------------------------------------------- *
 * mips_call & install_stub
 * --------------------------------------------------------------- */

int PQService::mips_call(std::string_view base_name, int param) {
    if (!transport_.is_open()) return -ENODEV;

    uint32_t params[11] = { 1, static_cast<uint32_t>(param) };
    uint32_t result[11] = { 0 };
    int ret = transport_.call(base_name, /*target_cpu=*/1, params, result);

    if (ret != 0 && opts_.verbose >= 2) {
        std::fprintf(stderr,
            "hy310-pqd: %s(%d) → %d (%s)\n",
            std::string(base_name).c_str(), param, ret,
            std::strerror(-ret));
    }
    return ret;
}

int PQService::install_stub(std::string_view base_name) {
    if (!transport_.is_open()) return -ENODEV;
    return transport_.install_routine(base_name, /*target_cpu=*/1);
}

/* --------------------------------------------------------------- *
 * Minimal daemon mode — socket command loop
 * --------------------------------------------------------------- */

int PQService::socket_server() {
    socket_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd_ < 0) return -errno;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, opts_.socket_path.c_str(),
                 sizeof(addr.sun_path) - 1);
    ::unlink(opts_.socket_path.c_str());
    if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return -errno;
    }
    ::chmod(opts_.socket_path.c_str(), 0666);
    ::listen(socket_fd_, 4);

    while (!stop_) {
        int cfd = ::accept(socket_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(cfd);
        ::close(cfd);
    }
    ::close(socket_fd_);
    ::unlink(opts_.socket_path.c_str());
    socket_fd_ = -1;
    return 0;
}

void PQService::handle_client(int fd) {
    char buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;

    /* Line format: "set KEY VALUE\n" or "get KEY\n" or "apply\n" */
    std::string line(buf);
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
    };
    trim(line);

    auto sp = line.find(' ');
    std::string cmd = (sp == std::string::npos) ? line : line.substr(0, sp);
    std::string args = (sp == std::string::npos) ? ""   : line.substr(sp + 1);

    char out[256] = "";
    int ret = 0;

    if (cmd == "apply") {
        ret = apply_all();
        std::snprintf(out, sizeof(out), "apply: %d\n", ret);
    } else if (cmd == "dump") {
        std::snprintf(out, sizeof(out),
            "brightness=%d contrast=%d sat=%d hue=%d sharp=%d backlight=%d "
            "colortemp=%d gamma=%d tnr=%d snr=%d dci=%d be=%d mode=%d src=%d\n",
            state_.brightness, state_.contrast, state_.saturation, state_.hue,
            state_.sharpness, state_.backlight, state_.colortemperature,
            state_.gamma, state_.tnr, state_.snr, state_.dci,
            state_.blackextension, state_.picture_mode, state_.source_type);
    } else if (cmd == "set") {
        auto sp2 = args.find(' ');
        if (sp2 == std::string::npos) {
            std::snprintf(out, sizeof(out), "usage: set KEY VALUE\n");
        } else {
            std::string k = args.substr(0, sp2);
            int v = std::atoi(args.substr(sp2 + 1).c_str());
            if      (k == "brightness")       ret = set_brightness(v);
            else if (k == "contrast")         ret = set_contrast(v);
            else if (k == "saturation")       ret = set_saturation(v);
            else if (k == "hue")              ret = set_hue(v);
            else if (k == "sharpness")        ret = set_sharpness(v);
            else if (k == "backlight")        ret = set_backlight(v);
            else if (k == "colortemperature") ret = set_colortemperature(v);
            else if (k == "gamma")            ret = set_gamma(v);
            else if (k == "tnr")              ret = set_tnr(v);
            else if (k == "snr")              ret = set_snr(v);
            else if (k == "dci")              ret = set_dci(v);
            else if (k == "blackextension")   ret = set_blackextension(v);
            else if (k == "picture_mode")     ret = set_picture_mode(v);
            else { std::snprintf(out, sizeof(out), "unknown key: %s\n", k.c_str()); ret = -ENOENT; }
            if (out[0] == 0) std::snprintf(out, sizeof(out), "set %s=%d: %d\n", k.c_str(), v, ret);
        }
    } else {
        std::snprintf(out, sizeof(out),
            "commands: apply | dump | set KEY VALUE\n");
    }
    ::write(fd, out, std::strlen(out));
}

int PQService::run() {
    if (start() != 0) return 1;
    return socket_server();
}

/* --------------------------------------------------------------- *
 * parse_args
 * --------------------------------------------------------------- */

int parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc)       out.config_dir  = argv[++i];
        else if (a == "--socket" && i + 1 < argc)  out.socket_path = argv[++i];
        else if (a == "--no-apply")                out.apply_initial = false;
        else if (a == "--no-stubs")                out.install_stubs = false;
        else if (a == "--no-upload")               out.upload_factory_gamma = false;
        else if (a == "--skip-lut")                out.skip_lut = true;
        else if (a == "--skip-mips-calls")         out.skip_mips_calls = true;
        else if (a == "--daemon")                  out.daemon_mode = true;
        else if (a == "--oneshot")                 out.daemon_mode = false;
        else if (a == "-v" || a == "--verbose")    out.verbose++;
        else if (a == "-q" || a == "--quiet")      out.verbose = 0;
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return -1;
        }
    }
    return 0;
}

} // namespace pqservice
} // namespace hy310
