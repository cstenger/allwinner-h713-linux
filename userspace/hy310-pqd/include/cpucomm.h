/*
 * hy310-pqd — Transport layer to MIPS via /dev/cpu_comm (FusionDale RPC)
 *
 * Clean-room reimplementation of libhalcpucomm.so.
 * Reference: /opt/hy310/stock-re/super_extracted/vendor/lib/libhalcpucomm.so
 *
 * Wire protocol verified from decompilation of:
 *   Trid_Util_CPUComm_Init          @ 0x46F5
 *   Trid_Util_CPUComm_InstallRoutine @ 0x448D / InstallRoutine2 @ 0x5F75
 *   CPUComm_CallEx                   @ 0x4C05
 *   Trid_Util_Name2ID                @ 0x1DC61 (libUtility.so)
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hy310 {
namespace cpucomm {

/* ---- wire-protocol constants (from stock RE) ------------------------- */

/* ioctl numbers on /dev/cpu_comm
 *
 * Stock uses 0xC0087F01 for sync CALL but our mainline kernel currently
 * exposes 0xC0087F26 (IOCTL_CALL) with identical payload semantics.
 * We default to mainline's. Override via build-time define if targeting
 * stock kernel. */
constexpr unsigned long IOCTL_CALL_MAINLINE = 0xC0087F26;  // our proto.c
constexpr unsigned long IOCTL_INSTALL_RT    = 0xC0087F30;  // install routine
constexpr unsigned long IOCTL_GET_CPUID     = 0x80087F34;  // get current cpu id
constexpr unsigned long IOCTL_QUERY_REGION  = 0xC0087F13;  // mmap region info
constexpr unsigned long IOCTL_CHECK_READY   = 0xC0087F21;  // slave-ready poll
constexpr unsigned long IOCTL_MALLOC        = 0xC0087F10;  // shared-mem alloc
constexpr unsigned long IOCTL_FREE          = 0x40047F11;  // shared-mem free

/* MIPS shared-memory physical base (from sunxi-mipsloader DTS).
 * The ShMem region is 5 MiB mapped 1:1 between ARM and MIPS.  Any pointer
 * obtained from IOCTL_MALLOC lives inside this window after userspace has
 * mmap'd it — see CpuComm::map_sharedmem(). */
constexpr uint32_t SHMEM_PHYS_BASE = 0x4E300000;
constexpr uint32_t SHMEM_SIZE      = 0x00500000;

/* CRC32 seed used by Trid_Util_Name2ID */
constexpr uint32_t NAME2ID_SEED = 0x00123456;

/* Install-routine descriptor: 96 bytes */
struct RoutineDesc {
    uint16_t channel;          // +0    channel index 0..4
    uint16_t reserved0;        // +2
    uint32_t pid;              // +4    process id
    uint32_t comp_id;          // +8    Name2ID(name_formatted)
    char     name[64];         // +12   "Name_%1x_%3.3x" pattern
    uint8_t  pad[16];          // +76..+91
    int32_t  next_index;       // +92   hash chain link (-1 = end)
} __attribute__((packed));
static_assert(sizeof(RoutineDesc) == 96, "RoutineDesc must be 96 bytes");

/* Call message buffer: 168 bytes — layout matches our mainline kernel's
 * IOCTL_CALL handler (cpu_comm_dev.c:181):
 *   CPUComm_Call(call_buf, &call_buf[64], &call_buf[120]);
 *              ^^^          ^^^^^^^^^^^    ^^^^^^^^^^^^
 *              msg header   params block    result block
 *
 * Layout is derived from proto.c::SendComm2CPUEx which reads/writes:
 *   +0..1  routine_name (u16, filled by FindRoutine)
 *   +2..3  dst_cpu u16 (remote-path reads here)
 *   +6..7  flags (u16, MSG_FLAG_NOTIFY/RETURN_ACK)
 *   +8..9  msg_type (u16, kernel writes 1=local, 2=remote)
 *   +10..11 slot_flags (u16, |= MSG_FLAG_SENT)
 *   +12..15 session_id (u32, kernel fills)
 *   +16..19 dst_cpu expanded (u32, kernel fills from FindRoutine)
 *   +24..27 dst_cpu expanded2 (u32)
 *   +28..31 pid
 *   +32..39 wait_obj (u64)
 *   +40..43 comp_id (u32) — CALLER MUST SET
 *   +44..63 (reserved)
 *   +64..103 params[10] (u32) — CALLER FILLS, CPUComm_Call reads
 *   +120..159 result area — kernel writes after ACK
 */
struct CallMsg {
    uint16_t routine_name_lo;  // +0
    uint16_t dst_cpu;          // +2
    uint16_t _rsvd_type;       // +4  (historical 'type', unused by kernel)
    uint16_t flags;            // +6  MSG_FLAG_NOTIFY / MSG_FLAG_RETURN_ACK
    uint16_t msg_type;         // +8  kernel writes 1 (local) or 2 (remote)
    uint16_t slot_flags;       // +10 kernel |= MSG_FLAG_SENT
    uint32_t session_id;       // +12
    uint32_t dst_cpu_expanded; // +16
    uint32_t _rsvd1;           // +20
    uint32_t dst_cpu_expanded2;// +24
    uint32_t pid;              // +28
    uint64_t wait_obj;         // +32
    uint32_t comp_id;          // +40
    uint32_t _rsvd2[5];        // +44..63
    uint32_t params[10];       // +64..103 — CPUComm_Call reads count here
    uint32_t _rsvd3[4];        // +104..119
    uint32_t result[10];       // +120..159 — result area
    uint32_t _rsvd4[2];        // +160..167
} __attribute__((packed));
static_assert(sizeof(CallMsg) == 168, "CallMsg must be 168 bytes");

/* ---- API ------------------------------------------------------------- */

/* Compute comp_id for a routine name using stock-compatible CRC32.
 *
 * Algorithm (from Trid_Util_Name2ID @ libUtility.so:0x1DC61):
 *   crc = 0x123456
 *   for byte b in name: crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
 *   return crc
 *
 * The polynomial is the standard Ethernet CRC32 (0xEDB88320). */
uint32_t name2id(std::string_view name);

/* Format routine name like stock: "BaseName_%1x_%3.3x" % (target_cpu, pid & 0xFFF)
 *
 * target_cpu is the CPU where the ROUTINE'S IMPLEMENTATION lives (not the
 * caller). For THal_Vp_* routines running on MIPS, pass 1 — this matches
 * what MIPS's hal_adapter_init registers.
 *
 * Example: ("THal_Vp_SetGamma", target_cpu=1, pid=0) → "THal_Vp_SetGamma_1_000" */
std::string format_routine_name(std::string_view base, unsigned target_cpu, unsigned pid);

class CpuComm {
public:
    CpuComm();
    ~CpuComm();

    /* Open /dev/cpu_comm and query the two shared regions (ioctl 0xC0087F13).
     * register_helpers=true also installs ResetNoticeCPU / ShowMM / checkCPUPluse
     * like Trid_Util_CPUComm_Init(1) does. */
    int open_device(bool register_helpers = false);
    void close_device();

    bool is_open() const { return fd_ >= 0; }
    int  fd()      const { return fd_; }

    /* Install a routine stub in the SharedMem hash table so FindRoutine
     * resolves calls to this name.
     *
     * target_cpu: where the actual implementation lives.
     *   0 = ARM  (for MIPS→ARM callbacks like ResetNoticeCPU)
     *   1 = MIPS (for THal_Vp_* routines dispatched via msgbox)
     *
     * The name is formatted as "<base>_<target_cpu>_000" and hashed with
     * Name2ID. This must match what the other side registers — MIPS's
     * hal_adapter_init registers all THal_Vp_* as "_1_000", so callers
     * must pass target_cpu=1 to get the matching hash.
     *
     * For MIPS routes the cpu_comm kernel module pre-registers the same
     * hashes at module load, so calling install_routine is idempotent
     * (safe duplicate). For ARM-lokale routes (callback stubs) install
     * IS required. */
    int install_routine(std::string_view name,
                        unsigned target_cpu,
                        unsigned channel = 0);

    /* High-level synchronous call by name.
     *   name: base routine name (e.g. "THal_Vp_SetBrightness")
     *   target_cpu: 0=ARM, 1=MIPS
     *   params_in[0] = count (≤ 10), params_in[1..count] = values
     * Returns 0 on ACK, negative errno on timeout/error. */
    int call(std::string_view name,
             unsigned        target_cpu,
             const uint32_t* params_in,
             uint32_t*       result_out);

    /* Low-level call when the comp_id hash is already known.
     * Used for direct-hash tests (test_mips_call.py style). */
    int call_by_id(uint32_t        comp_id,
                   unsigned        target_cpu,
                   const uint32_t* params_in,
                   uint32_t*       result_out);

    /* Low-level: check MIPS slave ready state (ioctl 0xC0087F21). */
    bool slave_ready();

    /* Get our own CPU id (0 = ARM). */
    unsigned get_cpu_id();

    /* ---- shared-memory helpers ----------------------------------
     *
     * The FusionDale protocol passes "phys addresses" of ARM-allocated
     * buffers into MIPS-side RPC calls (e.g. THal_Vp_Init expects a
     * phys pointer to a 55 KB buffer it will populate).
     *
     * To participate, userspace needs:
     *   1) allocate N bytes in the shared-mem pool (IOCTL_MALLOC returns
     *      a virtual address inside the kernel-mmap'd ShMem window).
     *   2) map that window into our address space once (/dev/mem at
     *      SHMEM_PHYS_BASE, 5 MiB).
     *   3) translate virt→phys via simple arithmetic using the window
     *      base.
     *   4) after use, IOCTL_FREE gives the buffer back.
     *
     * shared_malloc returns a pointer inside our mmap'd window (usable
     * by the CPU like a normal malloc), OR nullptr on failure.
     */
    int   map_sharedmem();              /* opens /dev/mem, mmaps 5 MiB */
    void  unmap_sharedmem();
    void* shared_malloc(uint32_t size);
    void  shared_free(void* vir);

    /* Translate a pointer inside the mmap'd ShMem window to its physical
     * address (what MIPS will see).  Returns 0 if out of range. */
    uint32_t virt_to_phys(const void* vir) const;

    /* ---- raw access to the ShMem window (for callers that want to
     *      parse MIPS-produced data directly) ---------------------- */
    void* sharedmem_base() const { return sharedmem_vir_; }
    size_t sharedmem_size() const { return SHMEM_SIZE; }

private:
    int fd_ = -1;
    unsigned cpu_id_ = 0;
    uint32_t session_pid_cookie_ = 0;

    /* /dev/mem mapping of the ShMem window.  Populated by map_sharedmem(). */
    int   mem_fd_         = -1;
    void* sharedmem_vir_  = nullptr;
};

} // namespace cpucomm
} // namespace hy310
