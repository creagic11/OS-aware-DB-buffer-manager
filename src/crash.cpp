#include "bufferscope/crash.hpp"

#include <csignal>
#include <unistd.h>

namespace bufferscope {

namespace detail {
std::atomic<int> g_armed_point{0};
static std::atomic<uint64_t> g_remaining{0};

bool crash_point_slow(CrashPoint p) {
    if (g_remaining.fetch_sub(1) != 1) return false;
    if (p == CrashPoint::PAGE_TORN_WRITE) return true;  // caller writes half a page, then dies
    die_now();
}
}  // namespace detail

const char* crash_point_name(CrashPoint p) {
    switch (p) {
        case CrashPoint::NONE: return "none";
        case CrashPoint::WAL_AFTER_APPEND: return "wal_after_append";
        case CrashPoint::WAL_AFTER_FLUSH: return "wal_after_flush";
        case CrashPoint::PAGE_BEFORE_WRITE: return "page_before_write";
        case CrashPoint::PAGE_TORN_WRITE: return "page_torn_write";
        case CrashPoint::CKPT_AFTER_PAGE_FLUSH: return "ckpt_after_page_flush";
        case CrashPoint::CKPT_AFTER_LOG_SWAP: return "ckpt_after_log_swap";
    }
    return "?";
}

void arm_crash(CrashPoint p, uint64_t nth) {
    detail::g_remaining.store(nth == 0 ? 1 : nth);
    detail::g_armed_point.store(static_cast<int>(p));
}

void disarm_crash() {
    detail::g_armed_point.store(0);
    detail::g_remaining.store(0);
}

void die_now() {
    ::kill(::getpid(), SIGKILL);
    ::_exit(137);  // unreachable in practice
}

}  // namespace bufferscope
