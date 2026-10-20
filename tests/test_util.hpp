#pragma once
// Shared helpers for the Phase 6 WAL / recovery / crash-injection tests.

#include "bufferscope/kv_store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace bstest {

namespace fs = std::filesystem;

/// Unique scratch directory removed on destruction.
struct ScratchDir {
    fs::path dir;
    explicit ScratchDir(const std::string& tag) {
        dir = fs::temp_directory_path() /
              ("bufferscope_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~ScratchDir() { fs::remove_all(dir); }
    std::string db(const std::string& name = "t.db") const { return (dir / name).string(); }
    static int& counter() {
        static int c = 0;
        return c;
    }
};

struct Op {
    bool is_put;
    std::string key;
    std::string value;
};

/// Deterministic mixed workload: 75% PUT (incl. overwrites), 25% DELETE.
inline Op make_op(uint64_t i, uint64_t seed = 99, int keys = 40) {
    uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ull ^ seed;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    Op op;
    op.key = "key_" + std::to_string(x % static_cast<uint64_t>(keys));
    op.is_put = (x >> 8) % 4 != 0;
    if (op.is_put) op.value = std::string(20 + (x >> 16) % 180, static_cast<char>('a' + i % 26)) + std::to_string(i);
    return op;
}

/// Expected state after applying ops [0, k).
inline std::map<std::string, std::string> oracle(uint64_t k, uint64_t seed = 99, int keys = 40) {
    std::map<std::string, std::string> m;
    for (uint64_t i = 0; i < k; ++i) {
        Op op = make_op(i, seed, keys);
        if (op.is_put) m[op.key] = op.value;
        else m.erase(op.key);
    }
    return m;
}

inline void apply(bufferscope::KVStore& s, const Op& op) {
    if (op.is_put) s.put(op.key, op.value);
    else s.remove(op.key);
}

/// Read the full live state of a store by probing the oracle key space.
inline std::map<std::string, std::string> dump(bufferscope::KVStore& s, int keys = 40) {
    std::map<std::string, std::string> m;
    for (int k = 0; k < keys; ++k) {
        std::string key = "key_" + std::to_string(k);
        if (auto v = s.get(key)) m[key] = *v;
    }
    return m;
}

struct ChildResult {
    bool killed = false;   // terminated by SIGKILL (an injected crash)
    int exit_code = -1;
    std::vector<uint64_t> messages;
};

/// Run `body(report_fd)` in a forked child. The child reports progress by writing
/// uint64_t values to report_fd. Returns how it died and everything it reported.
inline ChildResult run_in_child(const std::function<void(int)>& body) {
    int p[2];
    if (::pipe(p) != 0) throw std::runtime_error("pipe failed");
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(p[0]);
        try {
            body(p[1]);
        } catch (...) {
            ::_exit(3);
        }
        ::_exit(0);
    }
    ::close(p[1]);
    ChildResult r;
    uint64_t v;
    while (::read(p[0], &v, sizeof(v)) == static_cast<ssize_t>(sizeof(v))) r.messages.push_back(v);
    ::close(p[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    r.killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

inline void report(int fd, uint64_t v) {
    ssize_t n = ::write(fd, &v, sizeof(v));
    (void)n;
}

}  // namespace bstest
