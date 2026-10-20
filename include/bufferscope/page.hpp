#pragma once
/**
 * @file page.hpp
 * @brief Fixed 4096-byte page abstraction with header, deterministic
 *        serialization, and CRC32 checksum.
 *
 * Slotted page layout (4096 bytes total):
 *   [PageHeader (32 bytes)]
 *   [Slot directory (growing forward: slot 0, slot 1, ...)]
 *   [Free space]
 *   [Record payload (growing backward from page end: 4096)]
 *
 * PageHeader fields:
 *   magic            - 4 bytes, always 0xBF5C0E01
 *   page_id          - 4 bytes
 *   lsn              - 8 bytes (log sequence number / version)
 *   record_count     - 2 bytes
 *   free_space_offset- 2 bytes (offset to lowest-addressed record / boundary of free space)
 *   checksum         - 4 bytes (CRC32 over entire page excluding checksum field)
 *   reserved         - 8 bytes (padding to 32 bytes)
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <optional>
#include <string>
#include <stdexcept>

namespace bufferscope {

// Constants
constexpr uint32_t PAGE_SIZE       = 4096;
constexpr uint32_t PAGE_MAGIC      = 0xBF5C0E01;  // "BufferScope 01"
constexpr uint32_t HEADER_SIZE     = 32;
constexpr uint32_t PAYLOAD_OFFSET  = HEADER_SIZE;
constexpr uint32_t MAX_PAYLOAD     = PAGE_SIZE - HEADER_SIZE;
// Largest key+value byte total that fits in one empty page
// (4-byte length prefix + 4-byte slot entry are also needed).
constexpr uint32_t MAX_KV_BYTES    = MAX_PAYLOAD - 4 - 4;

// PageHeader
struct PageHeader {
    uint32_t magic            = PAGE_MAGIC;
    uint32_t page_id          = 0;
    uint64_t lsn              = 0;
    uint16_t record_count     = 0;
    uint16_t free_space_offset= PAGE_SIZE;  // Points to start of records (initially 4096)
    uint32_t checksum         = 0;
    uint64_t reserved         = 0;
};
static_assert(sizeof(PageHeader) == HEADER_SIZE,
              "PageHeader must be exactly 32 bytes");

// SlotEntry - each record in a page is located by a slot: (offset, length)
struct SlotEntry {
    uint16_t offset = 0;
    uint16_t length = 0;
};
static_assert(sizeof(SlotEntry) == 4, "SlotEntry must be 4 bytes");

// Page
class Page {
public:
    // Construct an empty page with a given page_id
    explicit Page(uint32_t page_id = 0);

    // Construct a page from raw 4096-byte data (deserialization)
    explicit Page(const std::byte* raw_data);

    // Prevent ambiguity: Page(0) should not match Page(const std::byte*)
    Page(std::nullptr_t) = delete;

    // Serialize the page to a 4096-byte buffer (deterministic)
    void serialize(std::byte* out) const;

    // Serialize to a self-contained array
    std::array<std::byte, PAGE_SIZE> serialize() const;

    // Compute CRC32 checksum over the entire page (checksum field zeroed)
    uint32_t compute_checksum() const;

    // Verify the stored checksum matches computed
    bool verify_checksum() const;

    // Update the stored checksum to match current content
    void update_checksum();

    // True if the header carries the BufferScope page magic number
    bool has_valid_magic() const;

    // Insert a fixed-length-with-length-prefix record.
    // Record format: [key_len:2][val_len:2][key_data][val_data]
    // Returns true on success, false if page is full.
    bool insert_record(const std::string& key, const std::string& value);

    // Overwrite the record in `slot_idx` in place if the new record is no larger than
    // the slot's current extent. Returns false (page unchanged) otherwise.
    bool update_record(uint16_t slot_idx, const std::string& key, const std::string& value);

    // True if insert_record(key, value) would succeed (no mutation).
    bool can_fit(size_t key_len, size_t value_len) const;

    // Read a record at a given slot index.
    std::optional<std::pair<std::string, std::string>> read_record(uint16_t slot_idx) const;

    // Delete a record at a given slot index (marks slot as empty, does NOT compact).
    bool delete_record(uint16_t slot_idx);

    // Accessors
    uint32_t page_id() const { return header_.page_id; }
    void set_page_id(uint32_t id) {
        header_.page_id = id;
        sync_header();
    }

    uint64_t lsn() const { return header_.lsn; }
    void set_lsn(uint64_t lsn) {
        header_.lsn = lsn;
        sync_header();
    }

    uint16_t record_count() const { return header_.record_count; }
    uint16_t free_space_offset() const { return header_.free_space_offset; }

    // How many bytes of free space remain in the payload area
    uint16_t free_space() const;

    // Direct access to raw data (for testing / file I/O)
    const std::byte* data() const { return data_.data(); }
    std::byte* mutable_data() { return data_.data(); }

private:
    PageHeader header_;
    std::array<std::byte, PAGE_SIZE> data_;

    void sync_header();
    SlotEntry get_slot(uint16_t idx) const;
    void set_slot(uint16_t idx, SlotEntry entry);
    uint16_t slot_directory_end() const;
};

// CRC32 utility (ISO 3309 / ITU-T V.42 polynomial)
uint32_t crc32(const std::byte* data, size_t length);

// In-place helpers on a raw 4096-byte page image (used by the buffer pool so
// frames can be checksummed/verified without copying into a Page object).
uint32_t compute_page_checksum(const std::byte* page_bytes);
void stamp_page_checksum(std::byte* page_bytes);
bool verify_page_bytes(const std::byte* page_bytes);  // magic + CRC32
uint32_t page_id_of(const std::byte* page_bytes);
uint64_t page_lsn_of(const std::byte* page_bytes);

}  // namespace bufferscope
