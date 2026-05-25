/*
 * receiver.cpp — MIPS->ARM callback receiver implementation.
 *
 * The kernel-side cpu_comm_user.c queues every incoming MIPS->ARM CALL
 * into per-fd ringbuffer of 32 messages. We block on read() in a
 * dedicated thread and dispatch by comp_id.
 *
 * On stop(), we set running_=false and shutdown(SHUT_RD)-equivalent by
 * relying on the signal-driven EINTR exit; the underlying fd is owned
 * by CpuComm, so we don't close it here.
 */
#include "receiver.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/select.h>
#include <unistd.h>

namespace hy310::cpucomm {

namespace {
constexpr size_t MSG_SIZE = 104;

/* Empty signal handler so SIGUSR1 wakes blocked read(2) without crashing. */
extern "C" void on_wakeup(int) {}
} // namespace

Receiver::Receiver(int fd) : fd_(fd), running_(false) {
    /* Install a no-op SIGUSR1 handler to wake read() on stop().
     * SA_RESTART is intentionally NOT set so read() returns EINTR. */
    struct sigaction sa{};
    sa.sa_handler = on_wakeup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, nullptr);
}

Receiver::~Receiver() {
    stop();
}

void Receiver::register_handler(uint32_t comp_id, Handler cb) {
    std::lock_guard<std::mutex> g(handlers_lock_);
    handlers_[comp_id] = std::move(cb);
}

void Receiver::set_default_handler(Handler cb) {
    std::lock_guard<std::mutex> g(handlers_lock_);
    default_handler_ = std::move(cb);
}

void Receiver::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&Receiver::thread_func, this);
}

void Receiver::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) {
        /* Use select with timeout to let thread observe running_=false.
         * SIGUSR1 to the thread also wakes the blocked read(). */
        pthread_kill(thread_.native_handle(), SIGUSR1);
        thread_.join();
    }
}

void Receiver::thread_func() {
    uint8_t buf[MSG_SIZE];

    /* Use select() with 500ms timeout so we can poll running_ even if
     * no messages arrive. read() is blocking on the kernel queue. */
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd_, &rfds);
        struct timeval tv{0, 500 * 1000}; /* 500ms */
        int sr = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (sr < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[receiver] select: %s\n", std::strerror(errno));
            break;
        }
        if (sr == 0 || !FD_ISSET(fd_, &rfds)) continue;

        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;
            std::fprintf(stderr, "[receiver] read: %s\n", std::strerror(errno));
            break;
        }
        if (n != static_cast<ssize_t>(MSG_SIZE)) {
            std::fprintf(stderr, "[receiver] short read: %zd\n", n);
            continue;
        }

        uint32_t comp_id = *reinterpret_cast<const uint32_t*>(buf + 40);

        Handler h;
        {
            std::lock_guard<std::mutex> g(handlers_lock_);
            auto it = handlers_.find(comp_id);
            if (it != handlers_.end())
                h = it->second;
            else if (default_handler_)
                h = default_handler_;
        }
        if (h) {
            try {
                h(buf, comp_id);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[receiver] handler exc: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "[receiver] handler unknown exc\n");
            }
        } else {
            std::fprintf(stderr, "[receiver] unhandled comp_id 0x%08x\n", comp_id);
        }
    }
}

} // namespace hy310::cpucomm
