/**
 * @file file_manager.cpp
 * @brief Implementation of page-oriented file I/O with BUFFERED and O_DIRECT modes.
 */

#include "bufferscope/file_manager.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <cassert>

namespace bufferscope {

FileManager::FileManager(const std::string& filepath, IOMode mode)
    : filepath_(filepath), mode_(mode) {

    int flags = O_RDWR | O_CREAT;

#ifdef __linux__
    if (mode_ == IOMode::DIRECT) {
        flags |= O_DIRECT;
    }
#endif

    fd_ = ::open(filepath.c_str(), flags, 0644);
    if (fd_ < 0) {
        if (errno == EINVAL && mode_ == IOMode::DIRECT) {
            throw std::runtime_error(
                "FileManager: open failed with O_DIRECT (EINVAL) on '" + filepath +
                "'. The underlying filesystem does not support O_DIRECT.");
        }
        throw std::runtime_error(
            "FileManager: failed to open '" + filepath + "': " +
            std::strerror(errno));
    }
}

FileManager::~FileManager() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FileManager::FileManager(FileManager&& other) noexcept
    : filepath_(std::move(other.filepath_)),
      fd_(other.fd_),
      mode_(other.mode_) {
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
    off_t offset = page_offset(page_id);

    ssize_t written = ::pwrite(fd_, aligned_buf, PAGE_SIZE, offset);

    if (written != static_cast<ssize_t>(PAGE_SIZE)) {
        if (errno == EINVAL && mode_ == IOMode::DIRECT) {
            throw std::runtime_error(
                "FileManager: O_DIRECT alignment error (EINVAL) writing page " +
                std::to_string(page_id) +
                ": buffer pointer, file offset, or transfer size is not aligned to the sector/page boundary.");
        }
        throw std::runtime_error(
            "FileManager: pwrite failed for page " +
            std::to_string(page_id) + ": " +
            std::strerror(errno));
    }
}

void FileManager::read_raw_aligned(uint32_t page_id, std::byte* aligned_buf) {
    assert(fd_ >= 0);
    off_t offset = page_offset(page_id);

    ssize_t bytes_read = ::pread(fd_, aligned_buf, PAGE_SIZE, offset);

    if (bytes_read != static_cast<ssize_t>(PAGE_SIZE)) {
        if (errno == EINVAL && mode_ == IOMode::DIRECT) {
            throw std::runtime_error(
                "FileManager: O_DIRECT alignment error (EINVAL) reading page " +
                std::to_string(page_id) +
                ": buffer pointer, file offset, or transfer size is not aligned to the sector/page boundary.");
        }
        throw std::runtime_error(
            "FileManager: pread failed for page " +
            std::to_string(page_id) + ": " +
            (bytes_read < 0 ? std::strerror(errno) : "short read"));
    }
}

void FileManager::write_page(Page& page) {
    // Update checksum before writing
    page.update_checksum();

    // Serialize into a 4096-byte aligned buffer (guaranteed aligned for O_DIRECT)
    alignas(4096) std::byte aligned_buf[PAGE_SIZE];
    page.serialize(aligned_buf);

    write_raw_aligned(page.page_id(), aligned_buf);
}

Page FileManager::read_page(uint32_t page_id) {
    // Aligned buffer for reading
    alignas(4096) std::byte aligned_buf[PAGE_SIZE];

    read_raw_aligned(page_id, aligned_buf);

    Page page(aligned_buf);

    // Verify page ID matches expected
    if (page.page_id() != page_id) {
        throw std::runtime_error(
            "FileManager: page_id mismatch when reading page " +
            std::to_string(page_id) + " (got " +
            std::to_string(page.page_id()) + ")");
    }

    return page;
}

void FileManager::flush() {
    assert(fd_ >= 0);

    if (::fdatasync(fd_) < 0) {
        throw std::runtime_error(
            "FileManager: fdatasync failed: " +
            std::string(std::strerror(errno)));
    }
}

uint32_t FileManager::page_count() const {
    assert(fd_ >= 0);

    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        throw std::runtime_error(
            "FileManager: fstat failed: " +
            std::string(std::strerror(errno)));
    }

    return static_cast<uint32_t>(st.st_size / PAGE_SIZE);
}

bool FileManager::page_exists(uint32_t page_id) const {
    return page_id < page_count();
}

off_t FileManager::page_offset(uint32_t page_id) {
    return static_cast<off_t>(page_id) * PAGE_SIZE;
}

}  // namespace bufferscope
