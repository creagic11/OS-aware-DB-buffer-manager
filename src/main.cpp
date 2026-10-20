/**
 * @file main.cpp
 * @brief BufferScope KV command-line entry point.
 *
 *   bufferscope [--db PATH] [--io-mode=buffered|direct] [--pool N] put KEY VALUE
 *   bufferscope [...] get KEY
 *   bufferscope [...] del KEY
 *   bufferscope [...] stats
 */

#include "bufferscope/kv_store.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace bufferscope;

static int usage() {
    std::cerr << "usage: bufferscope [--db PATH] [--io-mode=buffered|direct] [--pool N]\n"
                 "                   (put KEY VALUE | get KEY | del KEY | stats)\n";
    return 2;
}

int main(int argc, char** argv) {
    std::string db = "data/bufferscope.db";
    IOMode mode = IOMode::BUFFERED;
    size_t pool = 128;
    std::vector<std::string> cmd;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&](const std::string& flag) -> std::string {
                if (a.rfind(flag + "=", 0) == 0) return a.substr(flag.size() + 1);
                if (i + 1 >= argc) throw std::invalid_argument(flag + " needs a value");
                return argv[++i];
            };
            if (a == "--db" || a.rfind("--db=", 0) == 0) db = value("--db");
            else if (a == "--io-mode" || a.rfind("--io-mode=", 0) == 0) mode = parse_io_mode(value("--io-mode"));
            else if (a == "--pool" || a.rfind("--pool=", 0) == 0) pool = std::stoul(value("--pool"));
            else if (a == "-h" || a == "--help") return usage();
            else cmd.push_back(a);
        }
        if (cmd.empty()) return usage();

        auto parent = std::filesystem::path(db).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        KVStore store(db, pool, mode);

        if (cmd[0] == "put" && cmd.size() == 3) {
            store.put(cmd[1], cmd[2]);
            store.flush();
            std::cout << "OK\n";
        } else if (cmd[0] == "get" && cmd.size() == 2) {
            auto v = store.get(cmd[1]);
            if (!v) { std::cout << "(not found)\n"; return 1; }
            std::cout << *v << "\n";
        } else if ((cmd[0] == "del" || cmd[0] == "delete") && cmd.size() == 2) {
            bool ok = store.remove(cmd[1]);
            store.flush();
            std::cout << (ok ? "DELETED\n" : "(not found)\n");
            return ok ? 0 : 1;
        } else if (cmd[0] == "stats" && cmd.size() == 1) {
            const auto& m = store.buffer_pool().metrics();
            std::cout << "db=" << db << " io_mode=" << io_mode_name(mode) << " pool_frames=" << pool
                      << " pages=" << store.file_manager().page_count() << " live_keys=" << store.size()
                      << " startup_misses=" << m.get_misses() << "\n";
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
