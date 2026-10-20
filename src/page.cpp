/**
 * @file page.cpp
 * @brief Implementation of the fixed 4096-byte page abstraction.
 *
 * Checksum algorithm: CRC32 (ISO 3309 polynomial 0xEDB88320).
 * Slotted page architecture:
 * - Header at bytes [0..31]
 * - Slot directory at bytes [32 .. 32 + record_count * 4], growing forward
 * - Record data at [free_space_offset .. 4096], growing backward
 */

#include "bufferscope/page.hpp"
#include <algorithm>
#include <cassert>

namespace bufferscope {

// ── CRC32 lookup table (precomputed) ───────────────────────────────
static constexpr uint32_t CRC32_POLY = 0xEDB88320u;

// Slicing-by-8 tables: table[0] is the classic byte-wise table; table[k][i] is the
// CRC of byte i followed by k zero bytes. Processing 8 bytes per step gives the
// identical CRC32 (same polynomial/reflection/xor-out) ~6-8x faster than the
// byte-at-a-time loop.
static constexpr std::array<std::array<uint32_t, 256>, 8> make_crc_tables() {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) crc = (crc & 1) ? (crc >> 1) ^ CRC32_POLY : crc >> 1;
        t[0][i] = crc;
    }
    for (uint32_t i = 0; i < 256; ++i)
        for (int k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
    return t;
}

static constexpr auto crc_tables = make_crc_tables();
static constexpr const auto& crc_table = crc_tables[0];

// Continue a (pre-inverted) CRC over `length` bytes.
static uint32_t crc32_update(uint32_t crc, const std::byte* data, size_t length) {
    const auto* p = reinterpret_cast<const uint8_t*>(data);
    while (length >= 8) {
        uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= crc;  // little-endian host assumed (x86-64 / aarch64)
        crc = crc_tables[7][lo & 0xFF] ^ crc_tables[6][(lo >> 8) & 0xFF] ^ crc_tables[5][(lo >> 16) & 0xFF] ^
              crc_tables[4][lo >> 24] ^ crc_tables[3][hi & 0xFF] ^ crc_tables[2][(hi >> 8) & 0xFF] ^
              crc_tables[1][(hi >> 16) & 0xFF] ^ crc_tables[0][hi >> 24];
        p += 8;
        length -= 8;
    }
    while (length--) crc = (crc >> 8) ^ crc_table[(crc ^ *p++) & 0xFF];
    return crc;
}

uint32_t crc32(const std::byte* data, size_t length) {
    return crc32_update(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

// ── Raw page-image helpers ──────────────────────────────────────────

uint32_t compute_page_checksum(const std::byte* page_bytes) {
    // CRC32 over the page with the 4-byte checksum field treated as zero.
    constexpr size_t off = offsetof(PageHeader, checksum);
    static constexpr std::byte zeros[sizeof(uint32_t)] = {};
    uint32_t crc = crc32_update(0xFFFFFFFFu, page_bytes, off);
    crc = crc32_update(crc, zeros, sizeof(uint32_t));
    crc = crc32_update(crc, page_bytes + off + sizeof(uint32_t), PAGE_SIZE - off - sizeof(uint32_t));
    return crc ^ 0xFFFFFFFFu;
}

void stamp_page_checksum(std::byte* page_bytes) {
    uint32_t c = compute_page_checksum(page_bytes);
    std::memcpy(page_bytes + offsetof(PageHeader, checksum), &c, sizeof(c));
}

bool verify_page_bytes(const std::byte* page_bytes) {
    PageHeader h;
    std::memcpy(&h, page_bytes, sizeof(h));
    return h.magic == PAGE_MAGIC && h.checksum == compute_page_checksum(page_bytes);
}

uint32_t page_id_of(const std::byte* page_bytes) {
    uint32_t id;
    std::memcpy(&id, page_bytes + offsetof(PageHeader, page_id), sizeof(id));
    return id;
}

uint64_t page_lsn_of(const std::byte* page_bytes) {
    uint64_t lsn;
    std::memcpy(&lsn, page_bytes + offsetof(PageHeader, lsn), sizeof(lsn));
    return lsn;
}

// ── Page construction ──────────────────────────────────────────────

Page::Page(uint32_t page_id) : header_{} {
    header_.magic = PAGE_MAGIC;
    header_.page_id = page_id;
    header_.lsn = 0;
    header_.record_count = 0;
    header_.free_space_offset = static_cast<uint16_t>(PAGE_SIZE);
    header_.checksum = 0;
    header_.reserved = 0;

    // Zero the entire data buffer
    data_.fill(std::byte{0});

    sync_header();
    update_checksum();
}

bool Page::has_valid_magic() const {
    return header_.magic == PAGE_MAGIC;
}

Page::Page(const std::byte* raw_data) {
    // Copy raw data
    std::memcpy(data_.data(), raw_data, PAGE_SIZE);

    // Deserialize header
    std::memcpy(&header_, raw_data, HEADER_SIZE);
}

void Page::sync_header() {
    std::memcpy(data_.data(), &header_, HEADER_SIZE);
}

// ── Serialization ──────────────────────────────────────────────────

void Page::serialize(std::byte* out) const {
    // Start from internal data_ which holds records + slots
    std::memcpy(out, data_.data(), PAGE_SIZE);

    // Overwrite the header portion with the current header struct
    std::memcpy(out, &header_, HEADER_SIZE);
}

std::array<std::byte, PAGE_SIZE> Page::serialize() const {
    std::array<std::byte, PAGE_SIZE> buf;
    serialize(buf.data());
    return buf;
}

// ── Checksum ───────────────────────────────────────────────────────

uint32_t Page::compute_checksum() const {
    // Serialize to a temporary buffer
    auto buf = serialize();

    // Zero the checksum field (bytes 20-23 in the header)
    constexpr size_t checksum_offset = offsetof(PageHeader, checksum);
    std::memset(buf.data() + checksum_offset, 0, sizeof(uint32_t));

    // CRC32 over the full page
    return crc32(buf.data(), PAGE_SIZE);
}

bool Page::verify_checksum() const {
    return header_.checksum == compute_checksum();
}

void Page::update_checksum() {
    sync_header();
    header_.checksum = compute_checksum();
    // Also update the checksum in data_
    std::memcpy(data_.data() + offsetof(PageHeader, checksum),
                &header_.checksum, sizeof(uint32_t));
}

// ── Slot directory helpers ─────────────────────────────────────────

SlotEntry Page::get_slot(uint16_t idx) const {
    SlotEntry entry{};
    size_t offset = HEADER_SIZE + idx * sizeof(SlotEntry);
    std::memcpy(&entry, data_.data() + offset, sizeof(SlotEntry));
    return entry;
}

void Page::set_slot(uint16_t idx, SlotEntry entry) {
    size_t offset = HEADER_SIZE + idx * sizeof(SlotEntry);
    std::memcpy(data_.data() + offset, &entry, sizeof(SlotEntry));
}

uint16_t Page::slot_directory_end() const {
    return static_cast<uint16_t>(HEADER_SIZE + header_.record_count * sizeof(SlotEntry));
}

uint16_t Page::free_space() const {
    uint16_t slot_end = slot_directory_end();
    if (header_.free_space_offset <= slot_end) {
        return 0;
    }
    return static_cast<uint16_t>(header_.free_space_offset - slot_end);
}

// ── Record operations ──────────────────────────────────────────────

bool Page::insert_record(const std::string& key, const std::string& value) {
    // Record format: [key_len:2][val_len:2][key_data][val_data]
    // Guard against uint16_t truncation: a record (4-byte prefix + key + value)
    // plus its slot entry must fit in the payload area of a single page.
    if (key.size() + value.size() + 4 + sizeof(SlotEntry) > MAX_PAYLOAD) {
        return false;
    }
    uint16_t key_len = static_cast<uint16_t>(key.size());
    uint16_t val_len = static_cast<uint16_t>(value.size());
    uint16_t record_size = static_cast<uint16_t>(4 + key_len + val_len);  // 2+2 for length prefixes

    // Space check:
    // Need: 4 bytes for new SlotEntry at the slot directory
    //       record_size bytes at the payload area
    uint16_t new_slot_dir_end = static_cast<uint16_t>(
        HEADER_SIZE + (header_.record_count + 1) * sizeof(SlotEntry));

    if (header_.free_space_offset < record_size) {
        return false;
    }

    uint16_t new_record_offset = static_cast<uint16_t>(header_.free_space_offset - record_size);

    if (new_record_offset < new_slot_dir_end) {
        return false;  // Page is full
    }

    // Write the record data into data_ from new_record_offset
    size_t pos = new_record_offset;
    std::memcpy(data_.data() + pos, &key_len, 2); pos += 2;
    std::memcpy(data_.data() + pos, &val_len, 2); pos += 2;
    std::memcpy(data_.data() + pos, key.data(), key_len); pos += key_len;
    std::memcpy(data_.data() + pos, value.data(), val_len); pos += val_len;

    // Write new slot entry in slot directory
    SlotEntry slot{};
    slot.offset = new_record_offset;
    slot.length = record_size;
    set_slot(header_.record_count, slot);

    // Update header
    header_.free_space_offset = new_record_offset;
    header_.record_count++;
    sync_header();

    return true;
}

bool Page::update_record(uint16_t slot_idx, const std::string& key, const std::string& value) {
    if (slot_idx >= header_.record_count) return false;
    SlotEntry slot = get_slot(slot_idx);
    const size_t need = 4 + key.size() + value.size();
    if (slot.length == 0 || need > slot.length) return false;
    const uint16_t key_len = static_cast<uint16_t>(key.size());
    const uint16_t val_len = static_cast<uint16_t>(value.size());
    size_t pos = slot.offset;
    std::memcpy(data_.data() + pos, &key_len, 2); pos += 2;
    std::memcpy(data_.data() + pos, &val_len, 2); pos += 2;
    std::memcpy(data_.data() + pos, key.data(), key_len); pos += key_len;
    std::memcpy(data_.data() + pos, value.data(), val_len);
    slot.length = static_cast<uint16_t>(need);
    set_slot(slot_idx, slot);
    return true;
}

bool Page::can_fit(size_t key_len, size_t value_len) const {
    const size_t need = 4 + key_len + value_len;
    const size_t new_dir_end = HEADER_SIZE + (static_cast<size_t>(header_.record_count) + 1) * sizeof(SlotEntry);
    return need + sizeof(SlotEntry) <= MAX_PAYLOAD && header_.free_space_offset >= need &&
           header_.free_space_offset - need >= new_dir_end;
}

std::optional<std::pair<std::string, std::string>> Page::read_record(uint16_t slot_idx) const {
    if (slot_idx >= header_.record_count) {
        return std::nullopt;
    }

    SlotEntry slot = get_slot(slot_idx);
    if (slot.length == 0) {
        return std::nullopt;  // Deleted record
    }

    if (slot.offset < slot_directory_end() || slot.offset + slot.length > PAGE_SIZE) {
        return std::nullopt;  // Corrupted bounds
    }

    size_t pos = slot.offset;
    uint16_t key_len = 0, val_len = 0;
    std::memcpy(&key_len, data_.data() + pos, 2); pos += 2;
    std::memcpy(&val_len, data_.data() + pos, 2); pos += 2;

    if (4 + key_len + val_len > slot.length) {
        return std::nullopt;  // Corrupted record length prefix
    }

    std::string key(reinterpret_cast<const char*>(data_.data() + pos), key_len);
    pos += key_len;
    std::string value(reinterpret_cast<const char*>(data_.data() + pos), val_len);

    return std::make_pair(std::move(key), std::move(value));
}

bool Page::delete_record(uint16_t slot_idx) {
    if (slot_idx >= header_.record_count) {
        return false;
    }

    SlotEntry slot = get_slot(slot_idx);
    if (slot.length == 0) {
        return false;  // Already deleted
    }

    // Mark slot as deleted (zero length and offset)
    slot.length = 0;
    slot.offset = 0;
    set_slot(slot_idx, slot);

    return true;
}

}  // namespace bufferscope
