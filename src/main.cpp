/**
 * @file main.cpp
 * @brief BufferScope entry point (stub for Phase 0).
 *
 * A full CLI will be built in Phase 9. For now, this simply verifies
 * the page library works by writing and reading back a test page.
 */

#include "bufferscope/page.hpp"
#include "bufferscope/file_manager.hpp"
#include <iostream>
#include <filesystem>

int main() {
    std::cout << "BufferScope v0.1.0 — OS-Aware Database Buffer Manager\n";
    std::cout << "Phase 0: Disk Page Library\n\n";

    const std::string test_file = "data/test.db";
    std::filesystem::create_directories("data");

    try {
        // Create a page, insert a record, write it
        bufferscope::Page page(0u);
        page.insert_record("hello", "world");
        page.update_checksum();

        std::cout << "Page 0 created, record_count=" << page.record_count()
                  << ", free_space=" << page.free_space() << " bytes\n";

        // Write to file
        bufferscope::FileManager fm(test_file);
        fm.write_page(page);
        fm.flush();
        std::cout << "Page 0 written to " << test_file << "\n";

        // Read back
        auto read_back = fm.read_page(0u);
        if (read_back.verify_checksum()) {
            auto rec = read_back.read_record(0);
            if (rec) {
                std::cout << "Read back: key='" << rec->first
                          << "', value='" << rec->second << "'\n";
                std::cout << "Checksum: VALID\n";
            }
        } else {
            std::cerr << "ERROR: Checksum verification failed!\n";
            return 1;
        }

        std::cout << "\nPhase 0 smoke test: PASSED\n";
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
