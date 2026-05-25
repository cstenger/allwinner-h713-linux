/*
 * hy310-hdmird — HDMI-RX bringup daemon for HY310 mainline
 *
 * Replicates stock-android tvserver init sequence to bring HDMI-RX state
 * machine into Running state on the H713 SoC.
 *
 * Stock-RE source: /opt/hy310/stock-re/stock_routines.txt + stock_rpc_*.txt
 *
 * Architecture:
 *   1. Open /dev/cpu_comm + map shared memory (5 MiB)
 *   2. Load /lib/firmware/hdcp_v22.bin into shared mem
 *   3. Register 9 MipsHalCallback_* routines (no-op handlers; MIPS may
 *      need them registered for state advancement)
 *   4. Run stock boot init sequence (12 ARM→MIPS calls, see code below)
 *   5. Listen on /run/hy310-hdmird.sock for source-switch commands
 *   6. On switch: SetSource(N) + reapply picture-quality defaults
 */
#include "cpucomm.h"
#include "receiver.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

using namespace hy310::cpucomm;

namespace {

constexpr const char* HDCP22_FILE = "/lib/firmware/hdcp_v22.bin";
constexpr const char* SOCKET_PATH = "/run/hy310-hdmird.sock";
constexpr size_t HDCP22_SIZE       = 960;   /* file size on disk */
constexpr size_t HDCP22_STOCK_SIZE = 912;   /* size stock passes to MIPS (skip 48B header) */

/* Wce_SetWindow shmem buffer offsets — verified RE in libhaldisplay.so:0x5B5C
 *
 * THal_Vp_Wce_SetWindow takes 3 phys-pointers via cpu_comm:
 *   src_cfg  = WceWindow {h_start, h_size, v_start, v_size}  (16 bytes, 4 int32)
 *   dst_cfg  = same layout (16 bytes)
 *   aspect_ratio = enum int (4 bytes), 2 = 16:9 default
 *
 * We allocate 3 fixed slots in shmem after HDCP22 (which sits at +0x36000).
 * Slot 0x37000..0x3700F = src, 0x37020..0x3702F = dst, 0x37040..0x37043 = aspect.
 * Buffers are reused across init + source-switch calls. */
constexpr uint32_t WCE_SRC_OFFSET    = 0x37000;
constexpr uint32_t WCE_DST_OFFSET    = 0x37020;
constexpr uint32_t WCE_ASPECT_OFFSET = 0x37040;

/* CLI-controlled low_latency flag.
 * 0 = MIPS default (= stock behaviour, no SetLowLatencyMode call)
 * 1 = explicit low_latency on (extra call before Wce_SetWindow)
 * Set via --low-latency 0/1, default 0. */
int g_low_latency = 0;

volatile sig_atomic_t g_shutdown = 0;
void on_sigterm(int) { g_shutdown = 1; }

/* MipsHalCallback_* routines that MIPS expects ARM to register.
 * These are no-op stubs in our daemon — we only need MIPS to find
 * a registered listener so its calls don't error out. */
const char* kMipsCallbacks[] = {
    "MipsHalCallback_OnNewSPDPacket",
    "MipsHalCallback_SignalChange",
    "MipsHalCallback_OnNewGCPacket",
    "MipsHalCallback_HdmiHotPlugByPortHandler",
    "MipsHalCallback_OnNewAVIPacket",
    "MipsHalCallback_HdmiSignalValidCallback",
    "MipsHalCallback_OnNewACRPacket",
    "MipsHalCallback_DisplayLatencyChange",
    "MipsHalCallback_OnNewAudioInfoPacket",
    "MipsHalCallback_OnNewVSIPacket",
};

/*
 * CALL_GAP_MS — userspace throttle between IPC calls.
 *
 * WORKAROUND for FreeCall-FIFO drain in mainline cpu_comm. The 21-slot CALL
 * pool at (share_seq_w + 120) is only replenished MIPS-side via
 * Comm_ReleaseFreeCall after BG_Thread processes the call. ARM
 * SendComm2CPUEx step 9 polls Comm_GetFreeCall up to 100 times then bails
 * with -EBUSY, which IOCTL_CALL maps to -EFAULT (errno=14) for userspace.
 *
 * Without throttle: ARM outruns MIPS, drains pool after ~19-21 calls.
 * Stock-tvserver doesn't burst — UI events pace naturally.
 *
 * 500ms empirically chosen. Session-K-night documented 300ms working with
 * 28/28 calls, but with state-machine stuck at 3 (no advance to 4/5) MIPS
 * BG_Thread appears slower so we need more headroom.
 *
 * REAL FIX (TODO): kernel-side sem-wait in SendComm2CPUEx step 9. See
 * comment block tagged "SLOT-RELEASE-WAIT" in
 * drivers/soc/sunxi/cpu_comm/cpu_comm_proto.c.
 */
constexpr int CALL_GAP_MS = 50;  /* reduced from 500ms after Z-session IPC fixes */

bool call_routine(CpuComm& cc, const char* name,
                  std::initializer_list<uint32_t> args) {
    /* params layout: [0]=count, [1..]=args */
    uint32_t params[11] = {0};
    uint32_t result[11] = {0};
    if (args.size() > 9) {
        std::fprintf(stderr, "[hdmird] %s: too many args (%zu)\n", name, args.size());
        return false;
    }
    params[0] = static_cast<uint32_t>(args.size());
    size_t i = 1;
    for (auto v : args) params[i++] = v;

    /* Pace-throttle before issuing call — give cpu_comm FIFO time to drain. */
    std::this_thread::sleep_for(std::chrono::milliseconds(CALL_GAP_MS));

    auto t0 = std::chrono::steady_clock::now();
    int rc = cc.call(name, /*target_cpu=*/1, params, result);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    if (rc == 0) {
        std::fprintf(stderr, "[hdmird] %s(", name);
        bool first = true;
        for (auto v : args) {
            std::fprintf(stderr, "%s0x%x", first ? "" : ", ", v);
            first = false;
        }
        std::fprintf(stderr, ") OK in %lldms result=0x%x\n", (long long)ms, result[0]);
        return true;
    }
    std::fprintf(stderr, "[hdmird] %s FAILED rc=%d (%s) in %lldms\n",
                 name, rc, std::strerror(-rc), (long long)ms);
    return false;
}

/* Load HDCP22 key file into shared memory and return its physical
 * address (what MIPS will see).
 *
 * NOTE: mainline cpu_comm IOCTL_MALLOC fails (Trid_SMM_MallocAttr returns 0
 * because MIPS-side SMM heap isn't initialized — this is a known mainline
 * gap). We bypass SMM entirely and write into a fixed offset in the
 * shared-memory window, matching the address stock uses (0x4e336000 =
 * SHMEM_PHYS_BASE + 0x36000). MIPS dereferences whatever phys-addr we
 * pass, regardless of SMM bookkeeping. */
constexpr uint32_t HDCP22_OFFSET = 0x36000;  /* matches stock RPC log */

uint32_t load_hdcp22_key(CpuComm& cc) {
    std::ifstream f(HDCP22_FILE, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[hdmird] cannot open %s: %s\n",
                     HDCP22_FILE, std::strerror(errno));
        return 0;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (buf.size() != HDCP22_SIZE) {
        std::fprintf(stderr, "[hdmird] %s size=%zu, expected %zu\n",
                     HDCP22_FILE, buf.size(), HDCP22_SIZE);
        return 0;
    }
    /* Allwinner header check: first 4 bytes should be 5A A5 A5 5A */
    if (buf[0] != 0x5A || buf[1] != 0xA5 || buf[2] != 0xA5 || buf[3] != 0x5A) {
        std::fprintf(stderr, "[hdmird] %s missing Allwinner header\n", HDCP22_FILE);
        return 0;
    }

    void* base = cc.sharedmem_base();
    if (!base) {
        std::fprintf(stderr, "[hdmird] sharedmem not mapped\n");
        return 0;
    }
    /* Write key directly at the stock-compatible offset */
    void* shm = static_cast<uint8_t*>(base) + HDCP22_OFFSET;
    std::memcpy(shm, buf.data(), HDCP22_SIZE);

    uint32_t phys = SHMEM_PHYS_BASE + HDCP22_OFFSET;
    std::fprintf(stderr, "[hdmird] HDCP22 key loaded direct virt=%p phys=0x%08x (size=%zu)\n",
                 shm, phys, HDCP22_SIZE);
    return phys;
}

/* Helper: write 3 WceWindow buffers into shmem and call THal_Vp_Wce_SetWindow.
 *
 * Args mirror the WceWindow struct layout (h_start, h_size, v_start, v_size).
 * For 1080p60 passthrough use src = dst = (0, 1920, 0, 1080), aspect=2 (16:9).
 *
 * Verified via:
 *   - libhaldisplay.so::THal_Vp_Wce_SetWindow @ 0x5B5C (signature + Trid_Util_CPUComm_Malloc)
 *   - libtvpq.so::PQOverScan::doWceSetWindow @ 0x375C4 (3rd arg = aspect, NOT low_latency)
 *   - Stock-elog WCETop::SetWindow format string [%5d, %5d, %5d, %5d] = (h_start, h_size, v_start, v_size)
 */
bool wce_set_window(CpuComm& cc,
                    int src_h_start, int src_h_size, int src_v_start, int src_v_size,
                    int dst_h_start, int dst_h_size, int dst_v_start, int dst_v_size,
                    int aspect_ratio) {
    void* base = cc.sharedmem_base();
    if (!base) {
        std::fprintf(stderr, "[hdmird] wce_set_window: shmem not mapped\n");
        return false;
    }

    int32_t* src_buf = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(base) + WCE_SRC_OFFSET);
    int32_t* dst_buf = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(base) + WCE_DST_OFFSET);
    int32_t* asp_buf = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(base) + WCE_ASPECT_OFFSET);

    src_buf[0] = src_h_start; src_buf[1] = src_h_size;
    src_buf[2] = src_v_start; src_buf[3] = src_v_size;
    dst_buf[0] = dst_h_start; dst_buf[1] = dst_h_size;
    dst_buf[2] = dst_v_start; dst_buf[3] = dst_v_size;
    asp_buf[0] = aspect_ratio;

    uint32_t src_phys = SHMEM_PHYS_BASE + WCE_SRC_OFFSET;
    uint32_t dst_phys = SHMEM_PHYS_BASE + WCE_DST_OFFSET;
    uint32_t asp_phys = SHMEM_PHYS_BASE + WCE_ASPECT_OFFSET;

    std::fprintf(stderr, "[hdmird] wce_set_window src=(%d,%d,%d,%d) dst=(%d,%d,%d,%d) aspect=%d\n",
                 src_h_start, src_h_size, src_v_start, src_v_size,
                 dst_h_start, dst_h_size, dst_v_start, dst_v_size, aspect_ratio);

    return call_routine(cc, "THal_Vp_Wce_SetWindow", {src_phys, dst_phys, asp_phys});
}

bool register_mips_callbacks(CpuComm& cc) {
    /* target_cpu=0 → ARM-side routine (this process). MIPS sends calls
     * to comp_id with target_cpu=0 packed name.
     * We don't supply a callback function pointer in this version —
     * the kernel's cpu_comm dispatch will deliver via /dev/cpu_comm
     * read(). We don't actively read; calls fire-and-forget on MIPS
     * side per stock RPC log analysis (33 RETURNS / 0 sent back). */
    for (auto* name : kMipsCallbacks) {
        int rc = cc.install_routine(name, /*target_cpu=*/0);
        if (rc != 0) {
            std::fprintf(stderr, "[hdmird] install %s failed rc=%d (%s)\n",
                         name, rc, std::strerror(-rc));
            /* continue anyway — registration may not be load-bearing */
        } else {
            std::fprintf(stderr, "[hdmird] registered callback %s (id=0x%08x)\n",
                         name, name2id(format_routine_name(name, 0, 0)));
        }
    }
    return true;
}

/* Run the stock-android init sequence as captured from
 * /proc/cpu_comm/RPC_calls. Order matches sessionID 14-33.
 *
 * For arg-pointer calls (WhiteBalance, Wce_SetWindow) we don't have
 * factory data; we skip those for now and only run the directly-
 * parameterized calls. If MIPS state-advance depends on them, we'll
 * see in testing. */
void run_init_sequence(CpuComm& cc, uint32_t hdcp22_phys) {
    std::fprintf(stderr, "[hdmird] === stock init sequence ===\n");

    /* Vp_Init — explicit MIPS-side init (sessionID ~10, before picture defaults).
     * Stock log shows this as one of the earliest cpu_comm calls.
     *
     * 2026-05-05 fix: ParaCount=3 with Para[2] = phys-addr of a 64KB staging
     * region in shmem. MIPS handler (sub_8B109F04) does memcpy(dst=Para[2],
     * src=MIPS .bss @ 0x8B48C2F0, n=55296). Stock-LIVE-elog confirms
     * Session1(Call) ParaCount=3.
     *
     * Para[2]=0 → MIPS computes dst=0 → memcpy to MIPS-VA NULL → MMU fault →
     * BG_Thread breaks → cascading kernel-MM corruption (Y2 oops pattern).
     *
     * Staging region: shmem +4MB (0x4E700000), 64KB unused by cpu_comm.
     * MIPS overwrites with its .bss factory PQ data; we don't need to
     * pre-populate. Para[0] and Para[1] not read by handler — pass 0. */
    constexpr uint32_t kVpInitStagingPhys = SHMEM_PHYS_BASE + 0x00400000;  // 0x4E700000
    call_routine(cc, "THal_Vp_Init",                {0, 0, kVpInitStagingPhys});

    /* Subscribe MIPS to send signal-change + HPD events to ARM.
     *
     * 2026-05-05 fix: Wrapper-RE in libhaldisplay.so showed both APIs only
     * send ONE arg to MIPS — callback registration is ARM-side (in our case
     * via install_routine of MipsHalCallback_*_0_000). Stock-elog confirms
     * ParaCount=1 for both.
     *
     *   RegisterSignalChangeCallback(source_id):
     *     - source_id: 0..10 = specific source, 11 = all sources
     *     - libhaldisplay-wrapper: stores cb_ptr ARM-side, sends only source_id
     *     - We use 11 = "all sources" → MIPS subscribes for all
     *     (Stock makes 10 calls for 10 specific sources; 11 is equivalent for our use)
     *
     *   SetHDMIHotPlugByPortCallback(enable_flag):
     *     - 1 = enable HPD callbacks, 0 = disable
     *     - libhaldisplay-wrapper: if cb_ptr != NULL → sends 1, else 0
     *
     * The actual callback comp_id resolution happens via name-hash matching
     * when MIPS sends the callback CALL — our register_mips_callbacks()
     * pre-registers MipsHalCallback_*_0_000 routines to make that work. */
    call_routine(cc, "THal_Vp_RegisterSignalChangeCallback", {11});
    call_routine(cc, "THal_Vp_SetHDMIHotPlugByPortCallback", {1});

    /* Picture pipeline defaults (sessionID 14-22).
     * SetWhiteBalance skipped (needs WB-table data). */
    call_routine(cc, "Thal_Vp_SetBacklightLevel",  {0x64});
    call_routine(cc, "THal_Vp_SetBacklightWorkMode", {0x00});

    /* SetBacklightPwmInfo takes 4 args (period/duty/polarity/freq?) — we
     * don't have factory PWM values. Skip until we know the right struct. */
    /* call_routine(cc, "Thal_Vp_SetBacklightPwmInfo", {a1, a2, a3, a4}); */
    call_routine(cc, "THal_Vp_SetTNR",              {0x02});
    call_routine(cc, "THal_Vp_SetSNR",              {0x01});
    call_routine(cc, "THal_Vp_SetDCI",              {0x02});
    call_routine(cc, "THal_Vp_SetBlackExtension",   {0x01});
    call_routine(cc, "THal_Vp_SetPictureMode",      {0x01});
    call_routine(cc, "THal_Vp_SetVideoRange",       {0x00});

    /* Optional low_latency override (test-flag, --low-latency 1).
     * Stock init does NOT call SetLowLatencyMode — MIPS uses default.
     * We expose this for experimentation: low_latency=1 reduces internal
     * frame buffering at MIPS-side, may improve HDMI passthrough latency. */
    if (g_low_latency != 0) {
        std::fprintf(stderr, "[hdmird] enabling low_latency=%d (non-stock test path)\n", g_low_latency);
        call_routine(cc, "THal_Vp_SetLowLatencyMode", {static_cast<uint32_t>(g_low_latency)});
    }

    /* Wce_SetWindow #1 (sessionID 17) — initial window config before port-map.
     * Stock 1080p60 passthrough: src=dst=full 1920x1080, aspect=16:9 (=2).
     *
     * Triggers MIPS WindowManager::UpdateWce -> WCETop::SetWindow ->
     * 5x WinNode::CalcWindow + WriteReg pipeline = DE2 panel-output config.
     * SKIPPING THIS WAS THE LVDS-ROUTING BLOCKER through Z-session. */
    wce_set_window(cc,  /*src*/ 0, 1920, 0, 1080,
                       /*dst*/ 0, 1920, 0, 1080,
                       /*aspect*/ 2);

    call_routine(cc, "THal_Vp_CvbsSetPedestalMode", {0x01});

    /* HDMI port-init (sessionID 19-1b) — 3 port-map writes.
     * Stock-elog shows "Change Port(N) Map, from OLD to NEW" pairs:
     *   port 1 → map 0
     *   port 2 → map 1
     *   port 3 → map 2
     * libhaldisplay::THal_Vp_HDMI_SetPortMap takes (port, port_map) → ParaCount=2. */
    call_routine(cc, "THal_Vp_HDMI_SetPortMap",     {1, 0});
    call_routine(cc, "THal_Vp_HDMI_SetPortMap",     {2, 1});
    call_routine(cc, "THal_Vp_HDMI_SetPortMap",     {3, 2});

    /* Wce_SetWindow #2 (sessionID 1c) — re-apply after port-map. */
    wce_set_window(cc,  /*src*/ 0, 1920, 0, 1080,
                       /*dst*/ 0, 1920, 0, 1080,
                       /*aspect*/ 2);

    /* Audio pipeline (sessionID 1d-1f).
     * DisableBlackScreen has ParaCount=0 (no args). Wrapper-RE confirmed. */
    call_routine(cc, "THal_Vp_DisableBlackScreen",  {});
    call_routine(cc, "THal_Vp_TurnOnARCAudioPath",  {0x01});
    call_routine(cc, "THal_Vp_SwitchARCTXPath",     {0x00});

    /* HDCP22 key load (sessionID 20) — ParaCount=2 (phys, size).
     * Stock-elog logs `HDMIRX_SetHDCP22KeyData!!, size:912`.
     * File on disk is 960B, but stock passes 912 (skipping 48B header).
     * Wrapper-RE confirmed: Trid_Util_CPUComm_Call(name, {2, phys, size}, ...). */
    if (hdcp22_phys != 0) {
        call_routine(cc, "THal_Vp_SetHDCP22Key",    {hdcp22_phys, HDCP22_STOCK_SIZE});
    } else {
        std::fprintf(stderr, "[hdmird] WARNING: skipping SetHDCP22Key (no key)\n");
    }

    /* HPD timing (sessionID 21) */
    call_routine(cc, "THal_Vp_HDMI_SetHPDTimeInterval", {0xC8});

    std::fprintf(stderr, "[hdmird] === init sequence complete ===\n");
}

/* Picture-quality + window post-signal sequence (sessionIDs 35-45 from
 * stock_rpc_HDMI_SWITCHED.txt). Stock fires this AFTER MIPS reports
 * SignalChange(arg=3) — the WCE-stage progression depends on it.
 *
 * Reused for both initial source-switch and re-application on every
 * subsequent SignalChange(3). SetWhiteBalance is skipped (struct format
 * unknown — 3 doubles RGB gain). */
void run_post_signal_sequence(CpuComm& cc) {
    std::fprintf(stderr, "[hdmird] === post-signal sequence (sessions 35-45) ===\n");

    call_routine(cc, "Thal_Vp_SetBacklightLevel",    {0x64});
    /* SetWhiteBalance skipped (needs 24-byte struct = 3 doubles RGB gain, not yet derived) */
    call_routine(cc, "THal_Vp_SetTNR",               {0x02});
    call_routine(cc, "THal_Vp_SetSNR",               {0x01});
    call_routine(cc, "THal_Vp_SetDCI",               {0x02});
    call_routine(cc, "THal_Vp_SetBlackExtension",    {0x01});
    call_routine(cc, "THal_Vp_SetBacklightWorkMode", {0x00});
    call_routine(cc, "THal_Vp_SetPictureMode",       {0x01});

    /* DisableBlackScreen × 2 + Wce_SetWindow in between (sessionID 2b-2d).
     * ParaCount=0 for DisableBlackScreen (wrapper-RE confirmed). */
    call_routine(cc, "THal_Vp_DisableBlackScreen",   {});

    /* Wce_SetWindow #3 — final window config after source-switch. */
    wce_set_window(cc,  /*src*/ 0, 1920, 0, 1080,
                       /*dst*/ 0, 1920, 0, 1080,
                       /*aspect*/ 2);

    call_routine(cc, "THal_Vp_DisableBlackScreen",   {});

    std::fprintf(stderr, "[hdmird] === post-signal sequence complete ===\n");
}

/* Issue THal_Vp_SetSource only — the 11 follow-up picture-quality calls
 * are gated behind SignalChange(arg=3) callback (run_post_signal_sequence). */
void run_set_source(CpuComm& cc, uint32_t source) {
    std::fprintf(stderr, "[hdmird] === set-source %u ===\n", source);
    call_routine(cc, "THal_Vp_SetSource", {source});
}

/* Compatibility wrapper — issues SetSource + post-signal sequence
 * back-to-back without waiting for the SignalChange callback. Kept for
 * --no-receiver mode and the socket "src" command default. */
void run_source_switch(CpuComm& cc, uint32_t source) {
    run_set_source(cc, source);
    run_post_signal_sequence(cc);
}

/* Unix-socket command handler — minimal protocol:
 *   "src N\n"     → run_source_switch(N)
 *   "status\n"    → return "ready\n"
 *   "init\n"      → run_init_sequence again
 *   "quit\n"      → shutdown
 */
int run_socket_listener(CpuComm& cc, uint32_t hdcp22_phys) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return -1; }

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, SOCKET_PATH, sizeof(sa.sun_path) - 1);
    unlink(SOCKET_PATH);

    if (bind(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        perror("bind");
        close(sfd);
        return -1;
    }
    chmod(SOCKET_PATH, 0660);
    if (listen(sfd, 4) < 0) { perror("listen"); close(sfd); return -1; }

    std::fprintf(stderr, "[hdmird] listening on %s\n", SOCKET_PATH);

    while (!g_shutdown) {
        int cfd = accept(sfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        char buf[128];
        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            /* trim newline */
            for (ssize_t i = 0; i < n; ++i) if (buf[i] == '\n') buf[i] = 0;

            if (std::strncmp(buf, "src ", 4) == 0) {
                uint32_t src = std::atoi(buf + 4);
                run_source_switch(cc, src);
                dprintf(cfd, "ok\n");
            } else if (std::strcmp(buf, "status") == 0) {
                dprintf(cfd, "ready\n");
            } else if (std::strcmp(buf, "init") == 0) {
                run_init_sequence(cc, hdcp22_phys);
                dprintf(cfd, "ok\n");
            } else if (std::strcmp(buf, "quit") == 0) {
                dprintf(cfd, "bye\n");
                g_shutdown = 1;
            } else {
                dprintf(cfd, "err: unknown command\n");
            }
        }
        close(cfd);
    }

    close(sfd);
    unlink(SOCKET_PATH);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool no_init = false;
    bool no_callbacks = false;
    bool no_socket = false;
    bool no_receiver = false;        /* default: receiver active */
    int  post_signal_timeout_ms = 3000; /* fallback if SignalChange(3) never arrives */
    int  src_after_init = 3; /* SetSource(3) after init by default; 0 = skip */
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-init") no_init = true;
        else if (a == "--no-callbacks") no_callbacks = true;
        else if (a == "--no-socket") no_socket = true;
        else if (a == "--no-receiver") no_receiver = true;
        else if (a == "--no-src") src_after_init = 0;
        else if (a == "--src" && i+1 < argc) src_after_init = std::atoi(argv[++i]);
        else if (a == "--post-signal-timeout" && i+1 < argc) post_signal_timeout_ms = std::atoi(argv[++i]);
        else if (a == "--low-latency" && i+1 < argc) g_low_latency = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("hy310-hdmird [--no-init] [--no-callbacks] [--no-socket]\n"
                        "             [--no-receiver]                  disable MIPS->ARM callback loop\n"
                        "             [--post-signal-timeout MS]       default 3000 (fallback if no SignalChange(3))\n"
                        "             [--no-src | --src N]              default --src 3\n"
                        "             [--low-latency 0|1]               default 0 (= MIPS default)\n");
            return 0;
        }
    }

    signal(SIGTERM, on_sigterm);
    signal(SIGINT, on_sigterm);

    CpuComm cc;
    int r = cc.open_device(/*register_helpers=*/false);
    if (r != 0) {
        std::fprintf(stderr, "[hdmird] open /dev/cpu_comm failed: %s\n",
                     std::strerror(-r));
        return 1;
    }
    std::fprintf(stderr, "[hdmird] cpu_comm opened, cpu_id=%u slave_ready=%d\n",
                 cc.get_cpu_id(), cc.slave_ready() ? 1 : 0);

    /* Map shared memory pool */
    r = cc.map_sharedmem();
    if (r != 0) {
        std::fprintf(stderr, "[hdmird] map_sharedmem failed: %s\n",
                     std::strerror(-r));
        return 1;
    }
    std::fprintf(stderr, "[hdmird] shared mem mapped at %p (%zu bytes)\n",
                 cc.sharedmem_base(), cc.sharedmem_size());

    /* Load HDCP22 key */
    uint32_t hdcp22_phys = load_hdcp22_key(cc);

    /* Register MipsHalCallback_* routines so MIPS can deliver events */
    if (!no_callbacks) {
        register_mips_callbacks(cc);
    }

    /* Run stock init sequence */
    if (!no_init) {
        run_init_sequence(cc, hdcp22_phys);
        /* Stock pauses ~1s between init and source-switch. Give MIPS
         * time to settle + FIFO drain. */
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    /* === Receiver setup ===
     *
     * The cpu_comm-driver delivers incoming MIPS->ARM CALLs (e.g.
     * MipsHalCallback_SignalChange) via read() on /dev/cpu_comm
     * (kernel patch 2026-05-06 CC-night). We dispatch by comp_id and
     * gate the post-signal sequence on SignalChange(arg=3).
     *
     * The post-signal sequence MUST run only once per source-switch.
     * Stock RPC trace shows MIPS sends 2 SignalChange(3) callbacks back
     * to back; we need to dedupe. */
    Receiver receiver(cc.fd());

    std::mutex post_signal_mu;
    std::condition_variable post_signal_cv;
    std::atomic<bool> post_signal_done{false};

    if (!no_receiver) {
        const uint32_t SIGNAL_CHANGE_ID =
            name2id("MipsHalCallback_SignalChange_0_000");
        const uint32_t HOTPLUG_ID =
            name2id("MipsHalCallback_HdmiHotPlugByPortHandler_0_000");

        receiver.register_handler(SIGNAL_CHANGE_ID,
            [&](const uint8_t* msg, uint32_t /*comp_id*/) {
                /* Para[0] is at msg+44 (matches CallParam layout from
                 * cpucomm.cpp::call_by_id, params written into local_msg+44). */
                uint32_t state = *reinterpret_cast<const uint32_t*>(msg + 44);
                std::fprintf(stderr, "[hdmird] <- SignalChange state=%u\n", state);
                if (state == 3 && !post_signal_done.exchange(true)) {
                    std::fprintf(stderr, "[hdmird] -> firing post-signal sequence\n");
                    run_post_signal_sequence(cc);
                    {
                        std::lock_guard<std::mutex> g(post_signal_mu);
                    }
                    post_signal_cv.notify_all();
                }
            });

        receiver.register_handler(HOTPLUG_ID,
            [](const uint8_t* msg, uint32_t /*comp_id*/) {
                uint32_t port = *reinterpret_cast<const uint32_t*>(msg + 44);
                std::fprintf(stderr, "[hdmird] <- HdmiHotPlugByPortHandler port=%u\n", port);
            });

        receiver.set_default_handler(
            [](const uint8_t* msg, uint32_t comp_id) {
                uint32_t arg = *reinterpret_cast<const uint32_t*>(msg + 44);
                std::fprintf(stderr, "[hdmird] <- MipsHalCallback comp=0x%08x arg=0x%x\n",
                             comp_id, arg);
            });

        receiver.start();
        std::fprintf(stderr, "[hdmird] receiver started (cpu_comm callback dispatch)\n");
    }

    /* Auto source-switch (default to HDMI = 3 unless --no-src) */
    if (src_after_init != 0) {
        if (no_receiver) {
            /* Classic path: SetSource + post-signal back-to-back. */
            run_source_switch(cc, static_cast<uint32_t>(src_after_init));
        } else {
            /* Receiver-driven path: SetSource only, await SignalChange(3). */
            run_set_source(cc, static_cast<uint32_t>(src_after_init));

            std::unique_lock<std::mutex> lk(post_signal_mu);
            bool got = post_signal_cv.wait_for(
                lk, std::chrono::milliseconds(post_signal_timeout_ms),
                [&]{ return post_signal_done.load(); });
            if (!got) {
                std::fprintf(stderr,
                    "[hdmird] WARNING: SignalChange(3) timeout after %dms — "
                    "firing post-signal blindly\n", post_signal_timeout_ms);
                lk.unlock();
                if (!post_signal_done.exchange(true))
                    run_post_signal_sequence(cc);
            }
        }
    }

    /* Listen for commands (or just exit if --no-socket) */
    if (no_socket) {
        std::fprintf(stderr, "[hdmird] init+source done, exiting (--no-socket)\n");
    } else {
        run_socket_listener(cc, hdcp22_phys);
    }

    if (!no_receiver) {
        receiver.stop();
    }
    cc.unmap_sharedmem();
    cc.close_device();
    return 0;
}
