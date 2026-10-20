/**
 * @file file_manager.cpp
 * @brief Page-oriented file I/O with BUFFERED and O_DIRECT modes.
 */

#include "bufferscope/file_manager.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace bufferscope {

const char* io_mode_name(IOMode mode) {
    return mode == IOMode::DIRECT ? "direct" : "buffered";
}

IOMode parse_io_mode(const std::string& s) {
    if (s == "buffered") return IOMode::BUFFERED;
    if (s == "direct") return IOMode::DIRECT;
    throw std::invalid_argument("unknown --io-mode '" + s + "' (expected buffered|direct)");
}

namespace {

[[noreturn]] void throw_io(IOMode mode, const char* op, uint32_t page_id, int err) {
    if (err == EINVAL && mode == IOMode::DIRECT) {
        throw DirectIOAlignmentError(
            std::string("FileManager: O_DIRECT alignment error (EINVAL) ") + op + " page " +
                std::to_string(page_id) +
                ": buffer pointer, file offset, or transfer size is not aligned to the "
                "sector/page boundary (required alignment " +
                std::to_string(IO_ALIGNMENT) + " bytes).",
            err);
    }
    throw IOError(std::string("FileManager: ") + op + " failed for page " +
                      std::to_string(page_id) + ": " + std::strerror(err),
                  err);
}

}  // namespace

FileManager::FileManager(const std::string& filepath, IOMode mode)
    : filepath_(filepath), mode_(mode) {
    int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef __linux__
    if (mode_ == IOMode::DIRECT) flags |= O_DIRECT;
#else
    if (mode_ == IOMode::DIRECT)
        throw IOError("FileManager: O_DIRECT is only supported on Linux", ENOTSUP);
#endif

    const bool existed = std::filesystem::exists(filepath);
    fd_ = ::open(filepath.c_str(), flags, 0644);
    if (fd_ < 0) {
        int err = errno;
        if (err == EINVAL && mode_ == IOMode::DIRECT) {
            throw DirectIOAlignmentError(
                "FileManager: open failed with O_DIRECT (EINVAL) on '" + filepath +
                    "'. The underlying filesystem does not support O_DIRECT "
                    "(tmpfs/overlay mounts commonly reject it).",
                err);
        }
        throw IOError("FileManager: failed to open '" + filepath + "': " + std::strerror(err), err);
    }
    if (!existed) fsync_parent_dir(filepath);
}

FileManager::~FileManager() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FileManager::FileManager(FileManager&& other) noexcept
    : filepath_(std::move(other.filepath_)), fd_(other.fd_), mode_(other.mode_) {
    other.fd_ = -1;
}

FileManager& FileManager::operator=(FileManager&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        filepath_ = std::move(other.filepath_);
        fd_ = other.fd_;
        mode_ = other.mode_;
        other.fd_ = -1;
    }
    return *this;
}

void FileManager::write_raw_aligned(uint32_t page_id, const std::byte* aligned_buf) {
    assert(fd_ >= 0);
    const off_t offset = page_offset(page_id);
    size_t done = 0;
    while (done < PAGE_SIZE) {
        ssize_t n = ::pwrite(fd_, aligned_buf + done, PAGE_SIZE - done, offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_io(mode_, "writing", page_id, errno);
        }
        if (n == 0) throw IOError("FileManager: pwrite wrote 0 bytes for page " + std::to_string(page_id), EIO);
        done += static_cast<size_t>(n);
    }
    stats_.page_writes.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_written.fetch_add(PAGE_SIZE, std::memory_order_relaxed);
}

void FileManager::read_raw_aligned(uint32_t page_id, std::byte* aligned_buf) {
    assert(fd_ >= 0);
    const off_t offset = page_offset(page_id);
    size_t done = 0;
    while (done < PAGE_SIZE) {
        ssize_t n = ::pread(fd_, aligned_buf + done, PAGE_SIZE - done, offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_io(mode_, "reading", page_id, errno);
        }
        if (n == 0) {
            throw PageNotFoundError("FileManager: page " + std::to_string(page_id) +
                                        " is missing (short read at end of file '" + filepath_ + "')",
                                    page_id);
        }
        done += static_cast<size_t>(n);
    }
    stats_.page_reads.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_read.fetch_add(PAGE_SIZE, std::memory_order_relaxed);
}

void FileManager::write_page(Page& page) {
    page.update_checksum();
    alignas(IO_ALIGNMENT) std::byte aligned_buf[PAGE_SIZE];
    page.serialize(aligned_buf);
    write_raw_aligned(page.page_id(), aligned_buf);
}

Page FileManager::read_page(uint32_t page_id) {
    alignas(IO_ALIGNMENT) std::byte aligned_buf[PAGE_SIZE];
    read_raw_aligned(page_id, aligned_buf);
    Page page(aligned_buf);

    // Only trust the stored page_id when the page is intact; a corrupted header
    // is reported by the caller's checksum check, not as an id mismatch.
    if (page.verify_checksum() && page.page_id() != page_id) {
        throw PageIdMismatchError("FileManager: page_id mismatch when reading page " +
                                      std::to_string(page_id) + " (got " +
                                      std::to_string(page.page_id()) + ")",
                                  page_id, page.page_id());
    }
    return page;
}

Page FileManager::read_page_verified(uint32_t page_id) {
    Page page = read_page(page_id);
    if (!page.has_valid_magic() || !page.verify_checksum()) {
        throw ChecksumMismatchError("FileManager: checksum mismatch on page " + std::to_string(page_id) +
                                        " in '" + filepath_ + "' (torn write or corruption)",
                                    page_id);
    }
    return page;
}

void FileManager::flush() {
    assert(fd_ >= 0);
    auto t0 = std::chrono::steady_clock::now();
    if (::fdatasync(fd_) < 0) {
        int err = errno;
        throw IOError("FileManager: fdatasync failed: " + std::string(std::strerror(err)), err);
    }
    auto t1 = std::chrono::steady_clock::now();
    stats_.syncs.fetch_add(1, std::memory_order_relaxed);
    stats_.sync_ns.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
}

bool FileManager::drop_os_cache() {
    flush();
#ifdef __linux__
    return ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED) == 0;
#else
    return false;
#endif
}

void FileManager::truncate_pages(uint32_t n) {
    if (::ftruncate(fd_, page_offset(n)) < 0) {
        int err = errno;
        throw IOError("FileManager: ftruncate failed: " + std::string(std::strerror(err)), err);
    }
}

uint32_t FileManager::page_count() const {
    assert(fd_ >= 0);
    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        int err = errno;
        throw IOError("FileManager: fstat failed: " + std::string(std::strerror(err)), err);
    }
    return static_cast<uint32_t>(st.st_size / PAGE_SIZE);
}

bool FileManager::page_exists(uint32_t page_id) const {
    return page_id < page_count();
}

off_t FileManager::page_offset(uint32_t page_id) {
    return static_cast<off_t>(page_id) * PAGE_SIZE;
}

void fsync_parent_dir(const std::string& path) {
    std::filesystem::path p = std::filesystem::absolute(path).parent_path();
    int dfd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;  // best effort
    ::fsync(dfd);
    ::close(dfd);
}

}  // namespace bufferscope
