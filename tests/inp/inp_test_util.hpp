// Shared helpers for INP tests.
#pragma once

#include "abss/inp/deck.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace abss_test {

namespace fs = std::filesystem;

inline fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
inline std::string str(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

/// Unique temporary directory removed on destruction. Name contains a space on purpose.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        static std::atomic<int> counter{0};
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               u8("abss inp " + tag + " " + std::to_string(t) + "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const std::string& rel) const { return str(path / u8(rel)); }
};

inline void writeFile(const std::string& path, const std::string& content) {
    const fs::path p = u8(path);
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline std::string readFile(const std::string& path) {
    std::ifstream in(u8(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::string examplePath(const std::string& name) {
    return std::string(ABSS_EXAMPLES_DIR) + "/models/" + name;
}

/// 1-based line number of the first line equal to `text` (after trimming CR), 0 if absent.
inline int lineOf(const abss::inp::Deck& deck, int fileIndex, const std::string& text) {
    const auto& lines = deck.files()[fileIndex].lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string l = lines[i];
        while (!l.empty() && l.back() == '\r') l.pop_back();
        if (l == text) return static_cast<int>(i) + 1;
    }
    return 0;
}

inline const abss::ParameterInfo* findParam(const std::vector<abss::ParameterInfo>& ps, const std::string& name) {
    for (const auto& p : ps)
        if (p.name == name) return &p;
    return nullptr;
}

inline int countErrors(const abss::inp::Deck& d) {
    int n = 0;
    for (const auto& g : d.diagnostics())
        if (g.level == abss::inp::Diagnostic::Level::Error) ++n;
    return n;
}

inline bool hasDiagnostic(const abss::inp::Deck& d, const std::string& needle) {
    for (const auto& g : d.diagnostics())
        if (g.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace abss_test
