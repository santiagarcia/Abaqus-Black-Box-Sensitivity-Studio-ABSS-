// ABSS INP: private text and path helpers shared by deck/discovery/patch.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace abss::inp::detail {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string trim(std::string_view s);
std::string upper(std::string_view s);
/// Upper case, trimmed, internal whitespace runs collapsed to one space.
std::string normalizeName(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool startsWithI(std::string_view s, std::string_view prefix);
/// Remove one pair of surrounding double or single quotes.
std::string unquote(const std::string& s);

/// Length of a UTF-8 byte-order mark at the start of s (0 or 3).
std::size_t bomLength(std::string_view s);

/// Byte range [begin, end) of one comma-separated field's trimmed token inside a physical line.
/// An empty field has begin == end (placed right after the preceding comma/whitespace).
struct FieldSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};
/// All comma-separated fields of a data line, including an empty trailing field after a final comma.
std::vector<FieldSpan> fieldSpans(const std::string& line);

/// Split a keyword line body (after '*') at commas outside quotes.
std::vector<std::string> splitKeywordParts(const std::string& s);

/// Parse an integer label token ("12", "+12"); nullopt if not an integer.
std::optional<long long> parseInteger(const std::string& token);

/// Shortest decimal representation for display, e.g. "0.05", "210000".
std::string displayNumber(double v);

std::filesystem::path toPath(const std::string& utf8);
std::string fromPath(const std::filesystem::path& p);
/// Absolute, lexically normalized path (UTF-8).
std::string absoluteNormal(const std::string& utf8);
/// Case-insensitive on Windows, exact elsewhere.
bool samePath(const std::string& a, const std::string& b);

}  // namespace abss::inp::detail
