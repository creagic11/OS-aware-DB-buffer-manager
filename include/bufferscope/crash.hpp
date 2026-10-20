#pragma once
/**
 * @file crash.hpp
 * @brief Deterministic crash-injection points (Phase 6).
 *
 * Production code calls crash_point(CrashPoint::X) at well-defined places in the
 * durability protocol. Normally this is a single relaxed atomic load. A test
 * (usually in a fork()ed child) arms one point to fire on its N-th hit; when it
 * fires the process is killed with SIGKILL — no destructors, no flushes, exactly
 * like a crash. Whatever the process had handed to the kernel survives; whatever
 * was still in user-space buffers (buffer-pool frames, WAL buffer) is lost.
 *
 * Power-loss-style damage (torn pages, truncated files) is simulated separately by
 * the tests operating on the files after the child is dead.
 */

#include <atomic>
#include <cstdint>

namespace bufferscope {

enum class CrashPoint : int {
    NONE = 0,
    WAL_AFTER_APPEND,          // record buffered in the WAL, log not yet durable
    WAL_AFTER_FLUSH,           // (a) WAL durable, page changes not yet flushed
    PAGE_BEFORE_WRITE,         // (b) about to write a data page (eviction or flush)
    PAGE_TORN_WRITE,           //     half of a data page written, then crash
    CKPT_AFTER_PAGE_FLUSH,     // (c) pages durable, checkpoint metadata not yet updated
    CKPT_AFTER_LOG_SWAP,       //     new WAL installed, before returning
};

const char* crash_point_name(CrashPoint p);

/// Arm `p` to kill the process on its `nth` hit (1 = first hit).
void arm_crash(CrashPoint p, uint64_t nth = 1);
void disarm_crash();

namespace detail {
extern std::atomic<int> g_armed_point;
bool crash_point_slow(CrashPoint p);  // returns true if this hit should fire
}  // namespace detail

/// Returns true when the caller must perform a point-specific partial action
/// before dying (only PAGE_TORN_WRITE uses this); otherwise kills the process
/// itself if armed, or returns false.
inline bool crash_point(CrashPoint p) {
    if (detail::g_armed_point.load(std::memory_order_relaxed) != static_cast<int>(p)) return false;
    return detail::crash_point_slow(p);
}

/// Kill the current process immediately (SIGKILL).
[[noreturn]] void die_now();

}  // namespace bufferscope
