#pragma once
/**
 * @file file_manager.hpp
 * @brief Page-oriented file I/O using POSIX pread/pwrite/fdatasync and O_DIRECT.
 *
 * Stores pages at byte offset (page_id * PAGE_SIZE) in a single file.
 * Supports both BUFFERED (OS page cache) and DIRECT (O_DIRECT) I/O modes.
 *
 * Every physical read/write/sync is counted in IOStats so the buffered and
 * direct paths are instrumented identically (Phase 5 requirement).
 */

#include "bufferscope/errors.hpp"
#include "bufferscope/page.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace bufferscope {

// I/O mode enumeration
enum class IOMode {
    BUFFERED,   // Standard POSIX read/write (relies on OS page cache)
    DIRECT,     // Linux O_DIRECT (bypasses OS page cache, requires 4096-byte alignment)
};

const char* io_mode_name(IOMode mode);
IOMode parse_io_mode(const std::string& s);  // "buffered" | "direct"; throws on anything else

/// Alignment used for every direct-I/O buffer, offset and length.
/// Verified against the device in docs/ENVIRONMENT.md: max(logical sector, PAGE_SIZE).
constexpr size_t IO_ALIGNMENT = 4096;

/// Physical I/O counters (bytes actually handed to the kernel).
struct IOStats {
    std::atomic<uint64_t> page_reads{0};
    std::atomic<uint64_t> page_writes{0};
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<uint64_t> syncs{0};          // fdatasync calls
    std::atomic<uint64_t> sync_ns{0};        // total time spent in fdatasync

    void reset() {
        page_reads = 0; page_writes = 0; bytes_read = 0;
        bytes_written = 0; syncs = 0; sync_ns = 0;
    }
};

class FileManager {
public:
    /**
     * @brief Open (or create) a page file.
     * @throws DirectIOAlignmentError if O_DIRECT is rejected by the filesystem.
     * @throws IOError for any other open failure.
     */
    explicit FileManager(const std::string& filepath,
                         IOMode mode = IOMode::BUFFERED);

    ~FileManager();

    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;
    FileManager(FileManager&& other) noexcept;
    FileManager& operator=(FileManager&& other) noexcept;

    /// Write a page at page_id * PAGE_SIZE (checksum is refreshed first).
    void write_page(Page& page);

    /**
     * @brief Read a page from offset page_id * PAGE_SIZE without verifying it.
     *
     * Returns the raw page so callers/tests can inspect a corrupted page.
     * @throws PageNotFoundError if the page lies beyond end-of-file.
     * @throws PageIdMismatchError if the checksum is valid but the stored id differs
     *         (a page written to the wrong offset).
     */
    Page read_page(uint32_t page_id);

    /**
     * @brief Read and fully validate a page (checksum, magic, page id).
     * @throws ChecksumMismatchError, PageIdMismatchError, PageNotFoundError.
     */
    Page read_page_verified(uint32_t page_id);

    /**
     * @brief Raw transfers of exactly PAGE_SIZE bytes. In DIRECT mode the buffer
     *        must be IO_ALIGNMENT-aligned; violations surface as DirectIOAlignmentError.
     */
    void write_raw_aligned(uint32_t page_id, const std::byte* aligned_buf);
    void read_raw_aligned(uint32_t page_id, std::byte* aligned_buf);

    /// fdatasync the data file.
    void flush();

    /// Truncate the file to exactly n pages (used by crash-injection tests).
    void truncate_pages(uint32_t n);

    uint32_t page_count() const;
    bool page_exists(uint32_t page_id) const;

    int fd() const { return fd_; }
    const std::string& filepath() const { return filepath_; }
    IOMode mode() const { return mode_; }

    IOStats& stats() { return stats_; }
    const IOStats& stats() const { return stats_; }

    static off_t page_offset(uint32_t page_id);

private:
    std::string filepath_;
    int fd_ = -1;
    IOMode mode_;
    IOStats stats_;
};

/// fsync the directory containing `path` so a newly created file's entry is durable.
void fsync_parent_dir(const std::string& path);

}  // namespace bufferscope
