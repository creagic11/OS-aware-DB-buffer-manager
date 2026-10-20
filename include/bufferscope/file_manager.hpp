#pragma once
/**
 * @file file_manager.hpp
 * @brief Page-oriented file I/O using POSIX pread/pwrite/fdatasync and O_DIRECT (Phase 5).
 *
 * Stores pages at byte offset (page_id * PAGE_SIZE) in a single file.
 * Supports both BUFFERED (OS page cache) and DIRECT (O_DIRECT) I/O modes.
 */

#include "bufferscope/page.hpp"
#include <string>
#include <cstdint>
#include <cstddef>

namespace bufferscope {

// I/O mode enumeration
enum class IOMode {
    BUFFERED,   // Standard POSIX read/write (relies on OS page cache)
    DIRECT,     // Linux O_DIRECT (bypasses OS page cache, requires 4096-byte alignment)
};

class FileManager {
public:
    /**
     * @brief Open (or create) a page file.
     * @param filepath Path to the database file.
     * @param mode I/O mode (BUFFERED or DIRECT).
     */
    explicit FileManager(const std::string& filepath,
                         IOMode mode = IOMode::BUFFERED);

    ~FileManager();

    // Non-copyable, movable
    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;
    FileManager(FileManager&& other) noexcept;
    FileManager& operator=(FileManager&& other) noexcept;

    /**
     * @brief Write a page to the file at offset (page_id * PAGE_SIZE).
     * @param page The page to write (will be serialized with updated checksum).
     */
    void write_page(Page& page);

    /**
     * @brief Read a page from the file at offset (page_id * PAGE_SIZE).
     * @param page_id The page number to read.
     * @return The deserialized Page.
     * @throws std::runtime_error if read fails or checksum is invalid.
     */
    Page read_page(uint32_t page_id);

    /**
     * @brief Raw aligned buffer I/O for direct memory-to-disk transfers.
     */
    void write_raw_aligned(uint32_t page_id, const std::byte* aligned_buf);
    void read_raw_aligned(uint32_t page_id, std::byte* aligned_buf);

    /**
     * @brief Flush file data to stable storage (fdatasync).
     */
    void flush();

    /**
     * @brief Get the number of pages currently in the file.
     */
    uint32_t page_count() const;

    /**
     * @brief Check if a page exists in the file.
     */
    bool page_exists(uint32_t page_id) const;

    /**
     * @brief Get the file descriptor.
     */
    int fd() const { return fd_; }

    /**
     * @brief Get the filepath.
     */
    const std::string& filepath() const { return filepath_; }

    /**
     * @brief Get the configured I/O mode.
     */
    IOMode mode() const { return mode_; }

    // Static helper: compute byte offset for a given page_id
    static off_t page_offset(uint32_t page_id);

private:
    std::string filepath_;
    int fd_ = -1;
    IOMode mode_;
};

}  // namespace bufferscope
