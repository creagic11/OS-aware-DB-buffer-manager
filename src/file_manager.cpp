/**
 * @file file_manager.cpp
 * @brief Implementation of page-oriented file I/O using POSIX APIs.
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

    // Phase 5 will add: if (mode == IOMode::DIRECT) flags |= O_DIRECT;

    fd_ = ::open(filepath.c_str(), flags, 0644);
    if (fd_ < 0) {
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

void FileManager::write_page(Page& page) {
    assert(fd_ >= 0);

    // Update checksum before writing
    page.update_checksum();

    // Serialize
    auto buf = page.serialize();

    // Write at the correct offset
    off_t offset = page_offset(page.page_id());
    ssize_t written = ::pwrite(fd_, buf.data(), PAGE_SIZE, offset);

    if (written != static_cast<ssize_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "FileManager: pwrite failed for page " +
            std::to_string(page.page_id()) + ": " +
            std::strerror(errno));
    }
}

Page FileManager::read_page(uint32_t page_id) {
    assert(fd_ >= 0);

    std::array<std::byte, PAGE_SIZE> buf;
    off_t offset = page_offset(page_id);

    ssize_t bytes_read = ::pread(fd_, buf.data(), PAGE_SIZE, offset);

    if (bytes_read != static_cast<ssize_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "FileManager: pread failed for page " +
            std::to_string(page_id) + ": " +
            (bytes_read < 0 ? std::strerror(errno) : "short read"));
    }

    Page page(buf.data());

    // Verify magic number
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
