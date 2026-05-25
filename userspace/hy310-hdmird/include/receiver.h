/*
 * receiver.h — MIPS->ARM callback receiver for hy310-hdmird.
 *
 * Reads 104-byte CALL messages from /dev/cpu_comm via blocking read()
 * and dispatches them to user-registered handlers keyed by comp_id.
 *
 * Requires the cpu_comm-driver patch from 2026-05-06 CC-night that
 * adds .read/.poll on /dev/cpu_comm (cpu_comm_user.c).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace hy310::cpucomm {

class Receiver {
public:
    /* msg points to a 104-byte buffer. comp_id is the routing key
     * (already extracted from msg + 40). */
    using Handler = std::function<void(const uint8_t* msg, uint32_t comp_id)>;

    explicit Receiver(int fd);
    ~Receiver();

    /* Register a handler for a specific comp_id (Trid_Util_Name2ID).
     * Replaces any prior handler. Thread-safe. */
    void register_handler(uint32_t comp_id, Handler cb);

    /* Default handler invoked when no per-comp_id handler is registered. */
    void set_default_handler(Handler cb);

    /* Launch the receive thread. Idempotent. */
    void start();

    /* Stop the receive thread (closes fd-side pipe to unblock read). */
    void stop();

    bool running() const { return running_.load(); }

private:
    int fd_;
    std::thread thread_;
    std::atomic<bool> running_;
    std::mutex handlers_lock_;
    std::map<uint32_t, Handler> handlers_;
    Handler default_handler_;

    void thread_func();
};

} // namespace hy310::cpucomm
