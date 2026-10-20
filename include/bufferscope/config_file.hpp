#pragma once
/**
 * @file config_file.hpp
 * @brief `--config=FILE` support shared by the bufferscope and bench_runner CLIs.
 *
 * File format (one setting per line, '#' starts a comment):
 *
 *     io_mode = direct        # same names as the command-line flags,
 *     pool-mb = 64            # '_' and '-' are interchangeable
 *     scramble = true         # boolean flags: true/false
 *
 * Every line becomes "--key=value" and is inserted BEFORE the remaining command-line
 * arguments, so explicit flags on the command line override the file.
 */

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bufferscope {

struct ExpandedArgs {
    std::vector<std::string> args;
    size_t from_files = 0;  // args[0, from_files) came from config files
    auto begin() const { return args.begin(); }
    auto end() const { return args.end(); }
    size_t size() const { return args.size(); }
    const std::string& operator[](size_t i) const { return args[i]; }
};

inline ExpandedArgs expand_config_args(int argc, char** argv) {
    std::vector<std::string> from_files, rest;
    auto trim = [](std::string s) {
        const char* ws = " \t\r\n";
        s.erase(0, s.find_first_not_of(ws));
        s.erase(s.find_last_not_of(ws) + 1);
        return s;
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string path;
        if (a.rfind("--config=", 0) == 0) path = a.substr(9);
        else if (a == "--config" && i + 1 < argc) path = argv[++i];
        if (path.empty()) {
            rest.push_back(a);
            continue;
        }
        std::ifstream f(path);
        if (!f) throw std::invalid_argument("cannot open config file '" + path + "'");
        std::string line;
        int n = 0;
        while (std::getline(f, line)) {
            ++n;
            if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
            line = trim(line);
            if (line.empty()) continue;
            auto eq = line.find('=');
            if (eq == std::string::npos)
                throw std::invalid_argument(path + ":" + std::to_string(n) + ": expected key = value");
            std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
            for (char& c : key)
                if (c == '_') c = '-';
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
            if (val == "true") from_files.push_back("--" + key);
            else if (val != "false") from_files.push_back("--" + key + "=" + val);
        }
    }
    ExpandedArgs out;
    out.from_files = from_files.size();
    out.args = std::move(from_files);
    out.args.insert(out.args.end(), rest.begin(), rest.end());
    return out;
}

}  // namespace bufferscope
