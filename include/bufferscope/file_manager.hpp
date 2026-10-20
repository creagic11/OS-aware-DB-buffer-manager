#pragma once
/**
 * @file file_manager.hpp
 * @brief Page-oriented file I/O using POSIX pread/pwrite/fdatasync.
 *
 * Stores pages at byte offset (page_id * PAGE_SIZE) in a single file.
 * Phase 0: Only buffered POSIX I/O. O_DIRECT added in Phase 5.
 */

#include "bufferscope/page.hpp"
#include <string>
#include <cstdint>

namespace bufferscope {

// I/O mode enumeration (Phase 5 will add DIRECT)
enum class IOMode {
    BUFFERED,   // Standard POSIX read/write (relies on OS page cache)
    // DIRECT,  // O_DIRECT (Phase 5)
};

class FileManager {
public:
    /**
     * @brief Open (or create) a page file.
     * @param filepath Path to the database file.
     * @param mode I/O mode (only BUFFERED supported in Phase 0).
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
     * @throws std::runtime_error if the read fails or checksum is invalid.
     */
    Page read_page(uint32_t page_id);

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
     * @brief Get the file descriptor (for testing/advanced use).
     */
    int fd() const { return fd_; }

    /**
     * @brief Get the filepath.
     */
    const std::string& filepath() const { return filepath_; }

private:
    std::string filepath_;
    int fd_ = -1;
    IOMode mode_;

    // Compute byte offset for a given page_id
    static off_t page_offset(uint32_t page_id);
};

}  // namespace bufferscope
