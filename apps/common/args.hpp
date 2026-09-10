// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

// Dead-simple "--key value" / "--flag" parser for the demo executables.
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) != 0) continue;
            a = a.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                kv_[a] = argv[++i];
            } else {
                kv_[a] = "1";
            }
        }
    }

    [[nodiscard]] bool has(const std::string& k) const { return kv_.count(k) != 0; }

    [[nodiscard]] std::string str(const std::string& k, const std::string& d = "") const {
        auto it = kv_.find(k);
        return it == kv_.end() ? d : it->second;
    }
    [[nodiscard]] std::int64_t i64(const std::string& k, std::int64_t d) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? d : std::strtoll(it->second.c_str(), nullptr, 10);
    }
    [[nodiscard]] std::uint64_t u64(const std::string& k, std::uint64_t d) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? d : std::strtoull(it->second.c_str(), nullptr, 10);
    }
    [[nodiscard]] double f64(const std::string& k, double d) const {
        auto it = kv_.find(k);
        return it == kv_.end() ? d : std::strtod(it->second.c_str(), nullptr);
    }
    [[nodiscard]] bool flag(const std::string& k) const {
        auto it = kv_.find(k);
        return it != kv_.end() && it->second != "0" && it->second != "false";
    }

private:
    std::unordered_map<std::string, std::string> kv_;
};
