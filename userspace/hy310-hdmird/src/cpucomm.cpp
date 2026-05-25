/*
 * CpuComm — /dev/cpu_comm transport layer implementation.
 *
 * Mirrors the ioctl protocol used by stock libhalcpucomm.so but talks to
 * our mainline kernel's cpu_comm module. Message layout is identical;
 * only the CALL ioctl number differs (0xC0087F26 here vs 0xC0087F01 stock).
 */
#include "cpucomm.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace hy310 {
namespace cpucomm {

static constexpr const char* DEVICE_PATH    = "/dev/cpu_comm";
static constexpr int         SLAVE_WAIT_MS  = 20;
static constexpr int         SLAVE_MAX_POLL = 50;

/* ---- ctor/dtor ------------------------------------------------------- */

CpuComm::CpuComm() = default;

CpuComm::~CpuComm() {
    close_device();
}

/* ---- open / close ---------------------------------------------------- */

int CpuComm::open_device(bool register_helpers) {
    if (fd_ >= 0) return 0;

    fd_ = ::open(DEVICE_PATH, O_RDWR);
    if (fd_ < 0) {
        std::fprintf(stderr, "hy310-pqd: open(%s) failed: %s\n",
                     DEVICE_PATH, std::strerror(errno));
        return -errno;
    }

    /* Cache our CPU id and pid cookie used by the msg header. */
    cpu_id_             = get_cpu_id();
    session_pid_cookie_ = static_cast<uint32_t>(::getpid());

    /* Poll slave readiness once so callers get a useful status. Don't
     * block forever here — caller can retry on timeout. */
    for (int i = 0; i < SLAVE_MAX_POLL; ++i) {
        if (slave_ready()) break;
        usleep(SLAVE_WAIT_MS * 1000);
    }

    /* Stock's Init(1) installs these three helper routines. They give
     * MIPS a way to notify ARM about resets / request memory dumps /
     * run heartbeat checks. These are ARM-side callbacks — target_cpu=0
     * so they hash as "_0_000" and MIPS's upcall resolves to our stubs. */
    if (register_helpers) {
        install_routine("ResetNoticeCPU", /*target_cpu=*/0);
        install_routine("ShowMM",         /*target_cpu=*/0);
        install_routine("checkCPUPluse",  /*target_cpu=*/0);
    }
    return 0;
}

void CpuComm::close_device() {
    unmap_sharedmem();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

/* ---- shared-memory helpers ----------------------------------------- */

int CpuComm::map_sharedmem() {
    if (sharedmem_vir_) return 0;
    mem_fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd_ < 0) {
        std::fprintf(stderr, "hy310-pqd: open /dev/mem for ShMem: %s\n",
                     std::strerror(errno));
        return -errno;
    }
    sharedmem_vir_ = ::mmap(nullptr, SHMEM_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, mem_fd_, SHMEM_PHYS_BASE);
    if (sharedmem_vir_ == MAP_FAILED) {
        std::fprintf(stderr, "hy310-pqd: mmap(/dev/mem, ShMem 0x%x): %s\n",
                     SHMEM_PHYS_BASE, std::strerror(errno));
        sharedmem_vir_ = nullptr;
        ::close(mem_fd_);
        mem_fd_ = -1;
        return -errno;
    }
    return 0;
}

void CpuComm::unmap_sharedmem() {
    if (sharedmem_vir_ && sharedmem_vir_ != MAP_FAILED) {
        ::munmap(sharedmem_vir_, SHMEM_SIZE);
    }
    sharedmem_vir_ = nullptr;
    if (mem_fd_ >= 0) {
        ::close(mem_fd_);
        mem_fd_ = -1;
    }
}

void* CpuComm::shared_malloc(uint32_t size) {
    if (fd_ < 0)      return nullptr;
    if (!sharedmem_vir_) {
        /* Auto-map on first use. */
        if (map_sharedmem() != 0) return nullptr;
    }

    /* IOCTL_MALLOC layout: in-out u32[2].
     *   in:  params[1] = size
     *   out: params[0] = kernel-virtual address (inside the kernel's
     *        ShMem mapping — NOT directly usable by us; we translate
     *        it to a userspace pointer via the /dev/mem mmap window). */
    uint32_t params[2] = { 0, size };
    if (::ioctl(fd_, IOCTL_MALLOC, params) < 0) {
        std::fprintf(stderr, "hy310-pqd: IOCTL_MALLOC(size=%u): %s\n",
                     size, std::strerror(errno));
        return nullptr;
    }
    if (params[0] == 0) return nullptr;

    /* The kernel returns a kernel-virtual address.  We mapped the SAME
     * physical region via /dev/mem, so our userspace pointer is:
     *   user_vir = sharedmem_vir_ + (kvir - kernel_virt_base)
     *
     * We don't know kernel_virt_base directly, but the kernel's Mid2Vir
     * result is an offset into the ShMem region — in practice the low
     * bits of kvir are the offset from ShMem base.  For mainline kernel
     * we can derive the offset by assuming kvir is physical-like: its
     * low 24 bits match the physical offset.
     *
     * Simpler approach: ask the kernel via Convert2PhyAddr once, cache
     * the ARM-phys → kernel-vir delta.  Since Convert2PhyAddr isn't
     * implemented in this kernel, we fall back to treating the kvir's
     * low 24 bits as the offset into ShMem (0x00300000..0x008000000
     * window). */
    uint32_t kvir   = params[0];
    uint32_t offset = kvir & (SHMEM_SIZE - 1);  /* low 21 bits */
    if (offset + size > SHMEM_SIZE) {
        std::fprintf(stderr, "hy310-pqd: shared_malloc: kvir=0x%x maps out "
                             "of our window\n", kvir);
        return nullptr;
    }
    return static_cast<uint8_t*>(sharedmem_vir_) + offset;
}

void CpuComm::shared_free(void* vir) {
    if (fd_ < 0 || !vir) return;
    /* Kernel expects arg = kernel-virtual addr (the same kvir we got
     * back from IOCTL_MALLOC).  We can recover it from our mapped
     * pointer by the reverse offset arithmetic. */
    uint32_t offset = static_cast<uint32_t>(
        static_cast<const uint8_t*>(vir) - static_cast<const uint8_t*>(sharedmem_vir_));
    if (offset >= SHMEM_SIZE) return;

    /* Kernel's Vir2Mid() handles both kvir and phys-style inputs on
     * this platform; passing the phys equivalent works. */
    uint32_t arg = SHMEM_PHYS_BASE + offset;
    if (::ioctl(fd_, IOCTL_FREE, arg) < 0) {
        std::fprintf(stderr, "hy310-pqd: IOCTL_FREE(0x%x): %s\n",
                     arg, std::strerror(errno));
    }
}

uint32_t CpuComm::virt_to_phys(const void* vir) const {
    if (!sharedmem_vir_ || !vir) return 0;
    uint32_t offset = static_cast<uint32_t>(
        static_cast<const uint8_t*>(vir) - static_cast<const uint8_t*>(sharedmem_vir_));
    if (offset >= SHMEM_SIZE) return 0;
    return SHMEM_PHYS_BASE + offset;
}

/* ---- low-level helpers ---------------------------------------------- */

unsigned CpuComm::get_cpu_id() {
    if (fd_ < 0) return 0;
    uint32_t val = 0;
    /* ioctl 0x80087F34: 8-byte in-out, low 32 bits receives CPU id.
     * Stock loops while errno == EAGAIN (512). */
    int tries = 0;
    while (::ioctl(fd_, IOCTL_GET_CPUID, &val) < 0 && errno == 512 && ++tries < 100) {}
    return val & 0xF;
}

bool CpuComm::slave_ready() {
    if (fd_ < 0) return false;
    /* Stock sends a u32 with HIWORD=2 as "request status" sentinel and
     * reads back a signed int16 in the low half. >0 = ready.
     *
     * We mirror the 8-byte BYREF layout. */
    uint32_t payload[2] = { 0, 0x00020000 };   // [0] result, [1] hi=2 request
    if (::ioctl(fd_, IOCTL_CHECK_READY, payload) < 0 && errno != 512) {
        return false;
    }
    int16_t status = static_cast<int16_t>(payload[0] & 0xFFFF);
    return status > 0;
}

/* ---- install_routine ------------------------------------------------- */

int CpuComm::install_routine(std::string_view base_name,
                             unsigned target_cpu,
                             unsigned channel)
{
    if (fd_ < 0) return -EBADF;
    if (channel >= 5) return -EINVAL;   // stock enforces channel < 5
    if (target_cpu > 1) return -EINVAL;

    /* Format name using target_cpu (where the implementation lives), NOT
     * our own cpu_id_. MIPS registers "_1_000" for THal_Vp_*; ARM must
     * match that hash when pre-installing the proxy stub. */
    std::string name = format_routine_name(base_name, target_cpu, 0);
    uint32_t    cid  = name2id(name);

    RoutineDesc d{};
    d.channel    = static_cast<uint16_t>(channel);
    /* d.reserved0 doubles as the dst_cpu field that FindRoutine returns.
     * Without this, mainline AddInRoutine writes entry+2=0 (default) →
     * SendComm2CPUEx routes to ARM-self instead of MIPS. Stock-android
     * works because MIPS hal_adapter_init pre-registers all 81 routines
     * with entry+2=1; if MIPS hasn't (race/broken), ARM-side install
     * needs to carry target_cpu itself. 2026-05-04 Y2. */
    d.reserved0  = static_cast<uint16_t>(target_cpu);
    d.pid        = static_cast<uint32_t>(::getpid());
    d.comp_id    = cid;
    d.next_index = -1;
    std::strncpy(d.name, name.c_str(), sizeof(d.name) - 1);

    int r = ::ioctl(fd_, IOCTL_INSTALL_RT, &d);
    if (r < 0) {
        std::fprintf(stderr,
            "hy310-pqd: install_routine('%s', target_cpu=%u) failed: %s (comp_id=0x%08x)\n",
            name.c_str(), target_cpu, std::strerror(errno), cid);
        return -errno;
    }
    return 0;
}

/* ---- synchronous call by name (high-level) -------------------------- */

int CpuComm::call(std::string_view base_name,
                  unsigned        target_cpu,
                  const uint32_t* params_in,
                  uint32_t*       result_out)
{
    if (target_cpu > 1) return -EINVAL;
    std::string name = format_routine_name(base_name, target_cpu, 0);
    uint32_t    cid  = name2id(name);
    return call_by_id(cid, target_cpu, params_in, result_out);
}

/* ---- synchronous call by comp_id (low-level) ------------------------ */

int CpuComm::call_by_id(uint32_t        comp_id,
                        unsigned        target_cpu,
                        const uint32_t* params_in,
                        uint32_t*       result_out)
{
    if (fd_ < 0)            return -EBADF;
    if (!params_in)         return -EINVAL;
    if (params_in[0] > 10)  return -EINVAL;  // stock asserts count ≤ 10
    if (target_cpu > 1)     return -EINVAL;

    CallMsg msg{};
    msg.dst_cpu    = static_cast<uint16_t>(target_cpu);
    msg.flags      = 0;                  // no NOTIFY → wait for ACK
    msg.comp_id    = comp_id;
    msg.pid        = session_pid_cookie_;

    /* CPUComm_Call in kernel: first u32 of params block = count, rest = values. */
    const uint32_t n = params_in[0];
    msg.params[0] = n;
    for (uint32_t i = 0; i < n && i < 9; ++i) {
        msg.params[i + 1] = params_in[i + 1];
    }

    int r = ::ioctl(fd_, IOCTL_CALL_MAINLINE, &msg);
    if (r < 0) {
        /* ETIME(62) = MIPS didn't ACK.  ESRCH(3) = FindRoutine missed.
         * EFAULT(14) = buffer/pointer issue. */
        return -errno;
    }

    if (result_out) {
        result_out[0] = msg.result[0];
        for (uint32_t i = 0; i < 9; ++i)
            result_out[i + 1] = msg.result[i + 1];
    }
    return 0;
}

} // namespace cpucomm
} // namespace hy310
