#include "bufferscope/wal.hpp"

#include "bufferscope/crash.hpp"
#include "bufferscope/file_manager.hpp"  // fsync_parent_dir

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bufferscope {

namespace {

constexpr uint32_t MAX_RECORD_BYTES = sizeof(WalRecordHeader) + 2 * PAGE_SIZE;

[[noreturn]] void throw_errno(const std::string& what) {
    int err = errno;
    throw IOError("LogManager: " + what + ": " + std::strerror(err), err);
}

void write_all(int fd, const std::byte* data, size_t n, const std::string& path) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::write(fd, data + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw_errno("write to '" + path + "' failed");
        }
        done += static_cast<size_t>(w);
    }
}

}  // namespace

std::vector<std::byte> encode_wal_record(uint64_t lsn, WalRecordType type, uint8_t flags,
                                         uint32_t page_id, uint16_t slot, const void* key,
                                         uint32_t key_len, const void* val, uint32_t val_len) {
    WalRecordHeader h{};
    h.length = static_cast<uint32_t>(sizeof(WalRecordHeader) + key_len + val_len);
    h.lsn = lsn;
    h.type = static_cast<uint8_t>(type);
    h.flags = flags;
    h.slot = slot;
    h.page_id = page_id;
    h.key_len = key_len;
    h.val_len = val_len;

    std::vector<std::byte> out(h.length);
    std::memcpy(out.data(), &h, sizeof(h));
    if (key_len) std::memcpy(out.data() + sizeof(h), key, key_len);
    if (val_len) std::memcpy(out.data() + sizeof(h) + key_len, val, val_len);
    h.crc = crc32(out.data() + 8, h.length - 8);
    std::memcpy(out.data() + offsetof(WalRecordHeader, crc), &h.crc, sizeof(h.crc));
    return out;
}

WalScan LogManager::scan(const std::string& path) {
    WalScan s;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return s;
    struct stat st;
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        throw_errno("fstat");
    }
    std::vector<std::byte> buf(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t r = ::pread(fd, buf.data() + got, buf.size() - got, static_cast<off_t>(got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    ::close(fd);
    s.file_bytes = got;

    size_t off = 0;
    uint64_t expect_lsn = 0;
    while (off + sizeof(WalRecordHeader) <= got) {
        WalRecordHeader h;
        std::memcpy(&h, buf.data() + off, sizeof(h));
        if (h.length < sizeof(h) || h.length > MAX_RECORD_BYTES || off + h.length > got) break;
        if (static_cast<uint64_t>(h.key_len) + h.val_len + sizeof(h) != h.length) break;
        if (crc32(buf.data() + off + 8, h.length - 8) != h.crc) break;
        if (expect_lsn != 0 && h.lsn != expect_lsn) break;
        if (h.type < 1 || h.type > 5) break;

        WalRecord r;
        r.lsn = h.lsn;
        r.type = static_cast<WalRecordType>(h.type);
        r.flags = h.flags;
        r.page_id = h.page_id;
        r.slot = h.slot;
        const char* p = reinterpret_cast<const char*>(buf.data() + off + sizeof(h));
        r.key.assign(p, h.key_len);
        r.value.assign(p + h.key_len, h.val_len);
        off += h.length;
        r.end_offset = off;
        s.records.push_back(std::move(r));
        expect_lsn = h.lsn + 1;
    }
    s.valid_bytes = off;
    s.torn_tail = off != got;
    return s;
}

LogManager::LogManager(const std::string& path, uint32_t initial_page_count) : path_(path) {
    recovered_ = scan(path_);
    if (recovered_.records.empty()) {
        if (recovered_.file_bytes > 0) {
            throw StorageError("LogManager: '" + path_ +
                               "' is non-empty but has no valid CHECKPOINT header (corrupted log head)");
        }
        // Fresh log: install CHECKPOINT(lsn=1) atomically.
        next_lsn_ = 1;
        checkpoint(initial_page_count);
        recovered_ = scan(path_);
        return;
    }
    const WalRecord& first = recovered_.records.front();
    if (first.type != WalRecordType::CHECKPOINT) {
        throw StorageError("LogManager: '" + path_ + "' does not start with a CHECKPOINT record");
    }
    checkpoint_lsn_ = first.lsn;
    checkpoint_page_count_ = first.page_id;
    next_lsn_ = recovered_.records.back().lsn + 1;
    durable_lsn_ = next_lsn_ - 1;
    file_bytes_ = recovered_.valid_bytes;

    open_fd();
    if (recovered_.torn_tail) {
        // Cut the torn tail so new records are appended right after the valid prefix.
        if (::ftruncate(fd_, static_cast<off_t>(recovered_.valid_bytes)) < 0) throw_errno("ftruncate");
        if (::fdatasync(fd_) < 0) throw_errno("fdatasync");
    }
}

LogManager::~LogManager() {
    // Deliberately no flush: un-flushed records were never acknowledged as durable.
    if (fd_ >= 0) ::close(fd_);
}

void LogManager::open_fd() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd_ < 0) throw_errno("open '" + path_ + "'");
}

uint64_t LogManager::append(WalRecordType type, uint8_t flags, uint32_t page_id, uint16_t slot,
                            const void* key, uint32_t key_len, const void* val, uint32_t val_len) {
    const uint64_t lsn = next_lsn_++;
    auto rec = encode_wal_record(lsn, type, flags, page_id, slot, key, key_len, val, val_len);
    buffer_.insert(buffer_.end(), rec.begin(), rec.end());
    stats_.records.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes.fetch_add(rec.size(), std::memory_order_relaxed);
    crash_point(CrashPoint::WAL_AFTER_APPEND);
    return lsn;
}

uint64_t LogManager::append_put(uint32_t page_id, uint16_t slot, const std::string& key,
                                const std::string& value, bool commit) {
    return append(WalRecordType::PUT, commit ? WAL_FLAG_COMMIT : 0, page_id, slot, key.data(),
                  static_cast<uint32_t>(key.size()), value.data(), static_cast<uint32_t>(value.size()));
}

uint64_t LogManager::append_tombstone(uint32_t page_id, uint16_t slot, bool commit) {
    return append(WalRecordType::TOMBSTONE, commit ? WAL_FLAG_COMMIT : 0, page_id, slot, nullptr, 0,
                  nullptr, 0);
}

uint64_t LogManager::append_update(uint32_t page_id, uint16_t slot, const std::string& key,
                                   const std::string& value, bool commit) {
    return append(WalRecordType::UPDATE, commit ? WAL_FLAG_COMMIT : 0, page_id, slot, key.data(),
                  static_cast<uint32_t>(key.size()), value.data(), static_cast<uint32_t>(value.size()));
}

uint64_t LogManager::append_page_image(uint32_t page_id, const std::byte* image) {
    stats_.page_images.fetch_add(1, std::memory_order_relaxed);
    return append(WalRecordType::PAGE_IMAGE, 0, page_id, 0, nullptr, 0, image, PAGE_SIZE);
}

void LogManager::flush() {
    if (buffer_.empty()) return;
    auto t0 = std::chrono::steady_clock::now();
    write_all(fd_, buffer_.data(), buffer_.size(), path_);
    if (::fdatasync(fd_) < 0) throw_errno("fdatasync");
    auto t1 = std::chrono::steady_clock::now();
    file_bytes_ += buffer_.size();
    buffer_.clear();
    durable_lsn_ = next_lsn_ - 1;
    stats_.flushes.fetch_add(1, std::memory_order_relaxed);
    stats_.flush_ns.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    crash_point(CrashPoint::WAL_AFTER_FLUSH);
}

void LogManager::checkpoint(uint32_t page_count) {
    if (!buffer_.empty()) flush();  // never drop records that were appended

    const uint64_t lsn = next_lsn_;
    auto rec = encode_wal_record(lsn, WalRecordType::CHECKPOINT, 0, page_count, 0, nullptr, 0, nullptr, 0);
    const std::string tmp = path_ + ".tmp";
    int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (tfd < 0) throw_errno("open '" + tmp + "'");
    try {
        write_all(tfd, rec.data(), rec.size(), tmp);
        if (::fdatasync(tfd) < 0) throw_errno("fdatasync temp log");
    } catch (...) {
        ::close(tfd);
        throw;
    }
    ::close(tfd);
    if (::rename(tmp.c_str(), path_.c_str()) < 0) throw_errno("rename temp log");
    fsync_parent_dir(path_);

    open_fd();
    next_lsn_ = lsn + 1;
    durable_lsn_ = lsn;
    checkpoint_lsn_ = lsn;
    checkpoint_page_count_ = page_count;
    file_bytes_ = rec.size();
    stats_.flushes.fetch_add(1, std::memory_order_relaxed);
    crash_point(CrashPoint::CKPT_AFTER_LOG_SWAP);
}

}  // namespace bufferscope
