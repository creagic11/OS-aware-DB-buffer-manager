#pragma once
/**
 * @file errors.hpp
 * @brief Typed exceptions for the distinct storage failure modes.
 *
 * The specification requires several failure modes to be diagnosable on their
 * own rather than collapsed into a generic "I/O error":
 *   - O_DIRECT alignment violations (kernel EINVAL)           -> DirectIOAlignmentError
 *   - a page that is not present in the file (short read/EOF) -> PageNotFoundError
 *   - a page whose CRC32 does not match its contents          -> ChecksumMismatchError
 *   - a page stored at the wrong offset / with a wrong id     -> PageIdMismatchError
 *   - every buffer-pool frame pinned (no evictable victim)    -> BufferPoolExhaustedError
 *
 * All derive from std::runtime_error so older call sites that catch
 * std::runtime_error keep working.
 */

#include <cstdint>
#include <stdexcept>
#include <string>

namespace bufferscope {

class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IOError : public StorageError {
public:
    IOError(const std::string& msg, int err) : StorageError(msg), errno_(err) {}
    int error_code() const { return errno_; }

private:
    int errno_;
};

class DirectIOAlignmentError : public IOError {
public:
    using IOError::IOError;
};

class PageNotFoundError : public StorageError {
public:
    PageNotFoundError(const std::string& msg, uint32_t page_id)
        : StorageError(msg), page_id_(page_id) {}
    uint32_t page_id() const { return page_id_; }

private:
    uint32_t page_id_;
};

class ChecksumMismatchError : public StorageError {
public:
    ChecksumMismatchError(const std::string& msg, uint32_t page_id)
        : StorageError(msg), page_id_(page_id) {}
    uint32_t page_id() const { return page_id_; }

private:
    uint32_t page_id_;
};

class PageIdMismatchError : public StorageError {
public:
    PageIdMismatchError(const std::string& msg, uint32_t expected, uint32_t found)
        : StorageError(msg), expected_(expected), found_(found) {}
    uint32_t expected() const { return expected_; }
    uint32_t found() const { return found_; }

private:
    uint32_t expected_;
    uint32_t found_;
};

class BufferPoolExhaustedError : public StorageError {
public:
    using StorageError::StorageError;
};

}  // namespace bufferscope
