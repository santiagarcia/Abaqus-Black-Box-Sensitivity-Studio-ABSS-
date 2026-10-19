// ABSS INP: structure-preserving Abaqus input deck parser.
//
// Design: every physical line of every file is stored verbatim (SourceFile::lines) so the deck can be written
// back byte-identically; keyword blocks only reference lines by (file, line). The mesh, sets, steps and
// boundary conditions are derived views built after the raw keyword structure has been read.
#include "abss/inp/deck.hpp"

#include "inp_util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace abss::inp {

// ---------------------------------------------------------------------------------------------------------
// detail helpers (declared in inp_util.hpp)
// ---------------------------------------------------------------------------------------------------------
namespace detail {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string normalizeName(std::string_view s) {
    const std::string t = upper(trim(s));
    std::string out;
    out.reserve(t.size());
    bool prevSpace = false;
    for (char c : t) {
        if (isSpace(c)) {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
        if (x != y) return false;
    }
    return true;
}

bool startsWithI(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

std::string unquote(const std::string& s) {
    const std::string t = trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\'')))
        return t.substr(1, t.size() - 2);
    return t;
}

std::size_t bomLength(std::string_view s) {
    return (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
            static_cast<unsigned char>(s[2]) == 0xBF)
               ? 3
               : 0;
}

std::vector<FieldSpan> fieldSpans(const std::string& line) {
    std::vector<FieldSpan> spans;
    std::size_t start = bomLength(line);
    // Ignore a trailing CR (lines of files with mixed terminators keep it).
    std::size_t lineEnd = line.size();
    while (lineEnd > start && (line[lineEnd - 1] == '\r' || line[lineEnd - 1] == '\n')) --lineEnd;
    while (true) {
        std::size_t comma = line.find(',', start);
        const std::size_t segEnd = (comma == std::string::npos || comma > lineEnd) ? lineEnd : comma;
        std::size_t b = start, e = segEnd;
        while (b < e && isSpace(line[b])) ++b;
        while (e > b && isSpace(line[e - 1])) --e;
        spans.push_back({b, e});
        if (comma == std::string::npos || comma >= lineEnd) break;
        start = comma + 1;
    }
    return spans;
}

std::vector<std::string> splitKeywordParts(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            cur.push_back(c);
            if (c == quote) quote = 0;
        } else if (c == '"') {
            quote = c;
            cur.push_back(c);
        } else if (c == ',') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::optional<long long> parseInteger(const std::string& token) {
    std::string t = trim(token);
    if (!t.empty() && t[0] == '+') t.erase(0, 1);
    if (t.empty()) return std::nullopt;
    long long v = 0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc() || p != t.data() + t.size()) {
        // Accept integral floating tokens such as "5." used by some generators.
        auto d = parseNumber(t);
        if (d && std::isfinite(*d) && std::floor(*d) == *d && std::abs(*d) < 9e15) return static_cast<long long>(*d);
        return std::nullopt;
    }
    return v;
}

std::string displayNumber(double v) {
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec != std::errc()) return "?";
    return std::string(buf, p);
}

std::filesystem::path toPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string fromPath(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string absoluteNormal(const std::string& utf8) {
    std::error_code ec;
    std::filesystem::path p = toPath(utf8);
    std::filesystem::path a = std::filesystem::absolute(p, ec);
    if (ec) a = p;
    return fromPath(a.lexically_normal());
}

bool samePath(const std::string& a, const std::string& b) {
#ifdef _WIN32
    std::string x = a, y = b;
    std::replace(x.begin(), x.end(), '/', '\\');
    std::replace(y.begin(), y.end(), '/', '\\');
    return iequals(x, y);
#else
    return a == b;
#endif
}

}  // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------------------------------------
// Public free helpers
// ---------------------------------------------------------------------------------------------------------
std::optional<std::string> KeywordBlock::param(const std::string& keyUpper) const {
    const std::string k = normalizeName(keyUpper);
    for (const auto& [key, value] : params)
        if (key == k) return value;
    return std::nullopt;
}

bool KeywordBlock::hasParam(const std::string& keyUpper) const { return param(keyUpper).has_value(); }

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> out;
    const auto spans = fieldSpans(line);
    for (const auto& s : spans) out.push_back(line.substr(s.begin, s.end - s.begin));
    if (out.size() > 1 && out.back().empty()) out.pop_back();
    if (out.size() == 1 && out[0].empty()) out.clear();
    return out;
}

std::optional<double> parseNumber(const std::string& token) {
    std::string t = trim(token);
    if (t.empty()) return std::nullopt;
    if (t[0] == '+') t.erase(0, 1);
    if (t.empty()) return std::nullopt;
    // Fortran-style exponent letters.
    for (char& c : t)
        if (c == 'd' || c == 'D') c = 'E';
    // Reject anything that is not a plain decimal float (from_chars would also accept "inf"/"nan").
    for (char c : t) {
        const bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E';
        if (!ok) return std::nullopt;
    }
    double v = 0.0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v, std::chars_format::general);
    if (ec != std::errc() || p != t.data() + t.size()) return std::nullopt;
    return v;
}

// ---------------------------------------------------------------------------------------------------------
// DeckBuilder
// ---------------------------------------------------------------------------------------------------------
namespace {

std::string readFileBytes(const std::string& path, bool& ok) {
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

void splitLines(const std::string& text, SourceFile& sf) {
    std::size_t nl = 0, crlf = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') {
            ++nl;
            if (i > 0 && text[i - 1] == '\r') ++crlf;
        }
    sf.lineEnding = (nl > 0 && crlf == nl) ? "\r\n" : "\n";
    const bool stripCR = sf.lineEnding == "\r\n";
    sf.lines.clear();
    std::size_t start = 0;
    while (true) {
        const std::size_t p = text.find('\n', start);
        if (p == std::string::npos) {
            sf.lines.push_back(text.substr(start));
            break;
        }
        std::size_t e = p;
        if (stripCR && e > start && text[e - 1] == '\r') --e;
        sf.lines.push_back(text.substr(start, e - start));
        start = p + 1;
    }
    sf.finalNewline = !text.empty() && text.back() == '\n';
    if (sf.finalNewline) sf.lines.pop_back();  // the empty remainder after the final terminator
    if (text.empty()) sf.lines.clear();
}

bool isProcedureKeyword(const std::string& kw) {
    static const std::set<std::string> procs = {
        "*STATIC", "*DYNAMIC", "*VISCO", "*FREQUENCY", "*BUCKLE", "*HEAT TRANSFER",
        "*COUPLED TEMPERATURE-DISPLACEMENT", "*COUPLED THERMAL-ELECTRICAL", "*SOILS", "*STEADY STATE DYNAMICS",
        "*DIRECT CYCLIC", "*GEOSTATIC", "*RIKS", "*MODAL DYNAMIC", "*COMPLEX FREQUENCY", "*SUBSTRUCTURE GENERATE",
        "*MASS DIFFUSION", "*ANNEAL", "*ELECTROMAGNETIC", "*DYNAMIC TEMPERATURE-DISPLACEMENT", "*RANDOM RESPONSE",
        "*RESPONSE SPECTRUM", "*MATRIX GENERATE", "*STEADY STATE TRANSPORT", "*COUPLED THERMAL-ELECTROCHEMICAL"};
    return procs.count(kw) > 0;
}

/// Rodrigues rotation of x about the axis through a with direction (b - a), angle in degrees.
std::array<double, 3> rotateAbout(const std::array<double, 3>& x, const std::array<double, 3>& a,
                                  const std::array<double, 3>& b, double angleDeg) {
    std::array<double, 3> k{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double n = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    if (!(n > 0.0)) return x;
    for (double& c : k) c /= n;
    const double th = angleDeg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    const std::array<double, 3> v{x[0] - a[0], x[1] - a[1], x[2] - a[2]};
    const double kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    const std::array<double, 3> kxv{k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    std::array<double, 3> r{};
    for (int i = 0; i < 3; ++i) r[i] = a[i] + v[i] * c + kxv[i] * s + k[i] * kv * (1.0 - c);
    return r;
}

}  // namespace

class DeckBuilder {
public:
    Deck d;

    void loadRoot(const std::string& text, const std::string& path) {
        rootDir_ = fromPath(toPath(path).parent_path());
        const int fi = addFile(text, path, -1, 0);
        includeStack_.push_back(d.files_[fi].path);
        parseFile(fi);
        includeStack_.pop_back();
        finish();
    }

private:
    std::string rootDir_;
    std::vector<std::string> includeStack_;
    int currentData_ = -1;
    int stepCounter_ = -1;
    int currentStep_ = -1;
    std::string currentPart_;
    std::string currentInstance_;
    bool warnedParameter_ = false;

    void diag(Diagnostic::Level level, int fileIndex, int line, std::string msg) {
        Diagnostic g;
        g.level = level;
        g.file = fileIndex >= 0 && fileIndex < static_cast<int>(d.files_.size()) ? d.files_[fileIndex].path : "";
        g.line = line;
        g.message = std::move(msg);
        d.diagnostics_.push_back(std::move(g));
    }
    void warn(int fi, int line, std::string msg) { diag(Diagnostic::Level::Warning, fi, line, std::move(msg)); }
    void error(int fi, int line, std::string msg) { diag(Diagnostic::Level::Error, fi, line, std::move(msg)); }
    void info(int fi, int line, std::string msg) { diag(Diagnostic::Level::Info, fi, line, std::move(msg)); }

    int addFile(const std::string& text, const std::string& path, int fromFile, int fromLine) {
        SourceFile sf;
        sf.path = absoluteNormal(path);
        sf.includedFromFile = fromFile;
        sf.includedFromLine = fromLine;
        splitLines(text, sf);
        d.files_.push_back(std::move(sf));
        return static_cast<int>(d.files_.size()) - 1;
    }

    /// Resolve an INPUT= path. Abaqus resolves relative names against the job (current) directory, also inside
    /// nested includes (verified with Abaqus 2024 datacheck: "sub/a.inp" including "b.inp" finds ./b.inp and
    /// fails when b.inp exists only in sub/). ABSS assumes the job runs in the root deck's directory, so the
    /// root directory is tried first. As a convenience the including file's directory is tried next, with a
    /// warning because Abaqus itself would not find the file there.
    std::optional<std::string> resolveInclude(const std::string& raw, int fromFile, int fromLine) {
        const std::string name = unquote(raw);
        std::filesystem::path p = toPath(name);
        std::error_code ec;
        if (p.is_absolute()) {
            if (std::filesystem::is_regular_file(p, ec)) return absoluteNormal(name);
            return std::nullopt;
        }
        const std::filesystem::path c1 = toPath(rootDir_) / p;
        if (std::filesystem::is_regular_file(c1, ec)) return absoluteNormal(fromPath(c1));
        const std::filesystem::path c2 = toPath(d.files_[fromFile].path).parent_path() / p;
        if (std::filesystem::is_regular_file(c2, ec)) {
            warn(fromFile, fromLine,
                 "INPUT='" + name + "' was found only relative to the including file (" + fromPath(c2) +
                     "); Abaqus resolves relative INPUT= paths against the job directory and may not find it");
            return absoluteNormal(fromPath(c2));
        }
        return std::nullopt;
    }

    /// Load an external file referenced by INPUT= on block `bi`. For *INCLUDE the file is parsed as keyword
    /// input; for other keywords (e.g. *NODE, INPUT=) its lines are data lines of that block.
    void handleInput(int bi, int fromFile, int fromLine) {
        const KeywordBlock& kb = d.blocks_[bi];
        const auto input = kb.param("INPUT");
        if (!input || unquote(*input).empty()) {
            if (kb.keyword == "*INCLUDE") error(fromFile, fromLine, "*INCLUDE without INPUT= parameter");
            return;
        }
        const auto resolved = resolveInclude(*input, fromFile, fromLine);
        if (!resolved) {
            error(fromFile, fromLine, kb.keyword + ": INPUT file not found: " + unquote(*input));
            d.missing_.push_back(unquote(*input));
            return;
        }
        for (const auto& s : includeStack_)
            if (samePath(s, *resolved)) {
                error(fromFile, fromLine, "include cycle detected: " + *resolved + " is already being read");
                return;
            }
        bool ok = false;
        const std::string text = readFileBytes(*resolved, ok);
        if (!ok) {
            error(fromFile, fromLine, "cannot read INPUT file: " + *resolved);
            d.missing_.push_back(unquote(*input));
            return;
        }
        const int fi = addFile(text, *resolved, fromFile, fromLine);
        includeStack_.push_back(*resolved);
        if (kb.keyword == "*INCLUDE") {
            parseFile(fi);
        } else {
            const SourceFile& sf = d.files_[fi];
            for (int i = 0; i < static_cast<int>(sf.lines.size()); ++i) {
                const std::string s = trim(std::string_view(sf.lines[i]).substr(i == 0 ? bomLength(sf.lines[i]) : 0));
                if (s.empty() || s.rfind("**", 0) == 0) continue;
                if (s[0] == '*') {
                    warn(fi, i + 1, "keyword line ignored inside a data-only INPUT file of " + kb.keyword);
                    continue;
                }
                d.blocks_[bi].data.push_back({fi, i + 1, sf.lines[i]});
            }
        }
        includeStack_.pop_back();
    }

    void parseFile(int fi) {
        const int n = static_cast<int>(d.files_[fi].lines.size());
        for (int i = 0; i < n; ++i) {
            const std::string& raw = d.files_[fi].lines[i];
            const std::string s = trim(std::string_view(raw).substr(i == 0 ? bomLength(raw) : 0));
            if (s.empty()) continue;
            if (s.rfind("**", 0) == 0) continue;  // comment
            if (s[0] != '*') {
                if (currentData_ < 0) {
                    warn(fi, i + 1, "data line before any keyword ignored");
                } else {
                    d.blocks_[currentData_].data.push_back({fi, i + 1, raw});
                }
                continue;
            }
            // Keyword card, possibly continued on following lines when it ends with a comma.
            std::string card = s;
            int last = i;
            while (!card.empty() && card.back() == ',' && last + 1 < n) {
                int j = last + 1;
                while (j < n) {
                    const std::string t = trim(d.files_[fi].lines[j]);
                    if (!t.empty() && t.rfind("**", 0) != 0) break;
                    ++j;
                }
                if (j >= n) break;
                const std::string t = trim(d.files_[fi].lines[j]);
                if (t[0] == '*') break;  // a new keyword: the trailing comma was just sloppy
                card += " " + t;
                last = j;
            }
            const int bi = addKeyword(card, fi, i + 1, last - i + 1);
            i = last;
            const std::string& kw = d.blocks_[bi].keyword;
            if (kw == "*INCLUDE") {
                const int saved = currentData_;
                handleInput(bi, fi, d.blocks_[bi].line);
                currentData_ = saved;  // data lines after (or inside) an include continue the previous block
            } else if (d.blocks_[bi].hasParam("INPUT")) {
                currentData_ = bi;
                handleInput(bi, fi, d.blocks_[bi].line);
            } else {
                currentData_ = bi;
            }
        }
    }

    int addKeyword(const std::string& card, int fi, int line, int lineCount) {
        KeywordBlock kb;
        const auto parts = splitKeywordParts(card.substr(1));
        kb.keyword = "*" + normalizeName(parts.empty() ? std::string() : parts[0]);
        for (std::size_t k = 1; k < parts.size(); ++k) {
            const std::string p = trim(parts[k]);
            if (p.empty()) continue;
            const std::size_t eq = p.find('=');
            if (eq == std::string::npos) {
                kb.params.emplace_back(normalizeName(p), std::string());
            } else {
                kb.params.emplace_back(normalizeName(p.substr(0, eq)), trim(p.substr(eq + 1)));
            }
        }
        kb.fileIndex = fi;
        kb.line = line;
        kb.keywordLineCount = lineCount;

        const std::string& kw = kb.keyword;
        if (kw == "*STEP") {
            if (currentStep_ >= 0) warn(fi, line, "*STEP inside a step (missing *END STEP?)");
            currentStep_ = ++stepCounter_;
        }
        kb.stepIndex = currentStep_;
        if (kw == "*PART") {
            currentPart_ = unquote(kb.param("NAME").value_or(""));
            if (currentPart_.empty()) warn(fi, line, "*PART without NAME=");
        }
        kb.partName = currentPart_;
        if (kw == "*INSTANCE") currentInstance_ = unquote(kb.param("NAME").value_or(""));
        kb.instanceName = currentInstance_;

        if (kw == "*END STEP") {
            if (currentStep_ < 0) warn(fi, line, "*END STEP without *STEP");
            currentStep_ = -1;
        } else if (kw == "*END PART") {
            currentPart_.clear();
        } else if (kw == "*END INSTANCE") {
            currentInstance_.clear();
        } else if (kw == "*PARAMETER" && !warnedParameter_) {
            warnedParameter_ = true;
            warn(fi, line,
                 "*PARAMETER: parametrized decks are only partially supported (numeric *PARAMETER definitions are "
                 "exposed as custom parameters; expressions and <name> substitutions are not evaluated)");
        }
        d.blocks_.push_back(std::move(kb));
        return static_cast<int>(d.blocks_.size()) - 1;
    }

    // ------------------------------------------------------------------ derived views
    struct PendingConn {
        std::int32_t element;
        std::int32_t instance;
        std::vector<long long> labels;
        int fileIndex, line;
    };
    std::vector<PendingConn> pending_;
    std::unordered_map<NodeKey, std::int32_t, NodeKeyHash> elementIndex_;
    std::map<std::string, int> userElementNodes_;   // "U1" -> NODES=
    std::map<std::string, int> userElementCoords_;  // "U1" -> COORDINATES=
    std::map<std::string, std::vector<int>> partBlocks_;
    std::map<std::string, std::vector<std::int32_t>> instanceNodes_;
    std::set<std::pair<int, int>> warnedBlocks_;

    void finish() {
        // Heading.
        for (const auto& kb : d.blocks_)
            if (kb.keyword == "*HEADING") {
                std::string h;
                for (const auto& ln : kb.data) {
                    if (!h.empty()) h += "\n";
                    h += trim(ln.text);
                }
                d.heading_ = h;
                break;
            }
        buildSteps();
        buildMesh();
        if (currentStep_ >= 0) warn(0, 0, "last *STEP has no *END STEP");
        if (!currentPart_.empty()) warn(0, 0, "*PART " + currentPart_ + " has no *END PART");
    }

    void buildSteps() {
        for (int bi = 0; bi < static_cast<int>(d.blocks_.size()); ++bi) {
            const KeywordBlock& kb = d.blocks_[bi];
            if (kb.keyword == "*STEP") {
                StepInfo st;
                st.blockIndex = bi;
                st.name = unquote(kb.param("NAME").value_or(""));
                if (st.name.empty()) st.name = "Step-" + std::to_string(d.steps_.size() + 1);
                if (auto nl = kb.param("NLGEOM")) st.nlgeom = nl->empty() || iequals(*nl, "YES");
                if (auto pt = kb.param("PERTURBATION")) st.perturbation = pt->empty() || iequals(*pt, "YES");
                d.steps_.push_back(st);
            } else if (kb.keyword == "*END STEP") {
                if (!d.steps_.empty() && d.steps_.back().endBlockIndex < 0) d.steps_.back().endBlockIndex = bi;
            } else if (kb.stepIndex >= 0 && isProcedureKeyword(kb.keyword)) {
                if (d.steps_.empty()) continue;
                StepInfo& st = d.steps_.back();
                if (!st.procedure.empty()) continue;  // first procedure wins
                st.procedure = kb.keyword;
                if (kb.keyword == "*STATIC" || kb.keyword == "*VISCO" || kb.keyword == "*DYNAMIC" ||
                    kb.keyword == "*COUPLED TEMPERATURE-DISPLACEMENT" || kb.keyword == "*HEAT TRANSFER" ||
                    kb.keyword == "*SOILS" || kb.keyword == "*GEOSTATIC") {
                    st.fixedIncrementation = kb.hasParam("DIRECT");
                    std::vector<std::string> f;
                    if (!kb.data.empty()) f = splitFields(kb.data[0].text);
                    auto num = [&](std::size_t k) -> std::optional<double> {
                        return k < f.size() ? parseNumber(f[k]) : std::nullopt;
                    };
                    const auto ini = num(0), per = num(1), mn = num(2), mx = num(3);
                    st.timePeriod = per.value_or(1.0);
                    st.initialIncrement = ini.value_or(st.timePeriod);
                    st.minIncrement = mn.value_or(std::min(st.initialIncrement, 1e-5 * st.timePeriod));
                    st.maxIncrement = mx.value_or(st.timePeriod);
                    if (kb.keyword == "*STATIC" && kb.hasParam("RIKS")) st.procedure = "*STATIC, RIKS";
                }
            }
        }
        for (const auto& st : d.steps_)
            if (st.endBlockIndex < 0)
                warn(d.blocks_[st.blockIndex].fileIndex, d.blocks_[st.blockIndex].line,
                     "step '" + st.name + "' has no *END STEP");
    }

    std::int32_t instanceIndex(const std::string& nameUpper) const {
        for (std::size_t i = 0; i < d.mesh_.instanceNames.size(); ++i)
            if (iequals(d.mesh_.instanceNames[i], nameUpper)) return static_cast<std::int32_t>(i);
        return -1;
    }

    void warnOnce(const KeywordBlock& kb, const std::string& msg) {
        if (warnedBlocks_.insert({kb.fileIndex, kb.line}).second) warn(kb.fileIndex, kb.line, msg);
    }

    void buildMesh() {
        Mesh& m = d.mesh_;
        // User element node counts.
        for (const auto& kb : d.blocks_)
            if (kb.keyword == "*USER ELEMENT") {
                const std::string type = upper(unquote(kb.param("TYPE").value_or("")));
                if (auto n = kb.param("NODES")) {
                    if (auto v = parseInteger(*n)) userElementNodes_[type] = static_cast<int>(*v);
                }
                if (auto c = kb.param("COORDINATES")) {
                    if (auto v = parseInteger(*c)) userElementCoords_[type] = static_cast<int>(*v);
                }
            }
        // Part definitions are instantiated later.
        for (int bi = 0; bi < static_cast<int>(d.blocks_.size()); ++bi) {
            const KeywordBlock& kb = d.blocks_[bi];
            if (!kb.partName.empty() && kb.keyword != "*PART" && kb.keyword != "*END PART")
                partBlocks_[upper(kb.partName)].push_back(bi);
        }
        static const std::set<std::string> generators = {"*NGEN", "*NFILL", "*NCOPY", "*ELGEN", "*ELCOPY",
                                                          "*NMAP", "*NODAL THICKNESS", "*SYSTEM"};
        std::int32_t curInst = -1;
        std::array<double, 3> translation{0, 0, 0};
        std::vector<std::array<double, 7>> rotations;
        for (int bi = 0; bi < static_cast<int>(d.blocks_.size()); ++bi) {
            const KeywordBlock& kb = d.blocks_[bi];
            if (generators.count(kb.keyword))
                warn(kb.fileIndex, kb.line,
                     kb.keyword + " is not expanded by ABSS; the displayed mesh may be incomplete or untransformed");
            if (!kb.partName.empty()) continue;  // processed when instanced
            if (kb.keyword == "*INSTANCE") {
                const std::string name = unquote(kb.param("NAME").value_or(""));
                const std::string part = upper(unquote(kb.param("PART").value_or("")));
                curInst = static_cast<std::int32_t>(m.instanceNames.size());
                m.instanceNames.push_back(name);
                translation = {0, 0, 0};
                rotations.clear();
                for (std::size_t li = 0; li < kb.data.size(); ++li) {
                    const auto f = splitFields(kb.data[li].text);
                    if (li == 0) {
                        for (std::size_t k = 0; k < 3 && k < f.size(); ++k) translation[k] = parseNumber(f[k]).value_or(0.0);
                    } else {
                        std::array<double, 7> r{};
                        for (std::size_t k = 0; k < 7 && k < f.size(); ++k) r[k] = parseNumber(f[k]).value_or(0.0);
                        rotations.push_back(r);
                    }
                }
                if (!kb.hasParam("PART") && !kb.hasParam("INSTANCE") && !kb.hasParam("LIBRARY"))
                    warn(kb.fileIndex, kb.line, "*INSTANCE without PART=");
                auto it = partBlocks_.find(part);
                if (it == partBlocks_.end()) {
                    if (!part.empty()) error(kb.fileIndex, kb.line, "*INSTANCE refers to unknown part " + part);
                } else {
                    for (int pb : it->second) processMeshBlock(d.blocks_[pb], curInst, upper(name) + ".");
                }
                continue;
            }
            if (kb.keyword == "*END INSTANCE") {
                if (curInst >= 0) applyTransform(curInst, translation, rotations);
                curInst = -1;
                continue;
            }
            if (!kb.instanceName.empty() && curInst >= 0) {
                processMeshBlock(kb, curInst, upper(kb.instanceName) + ".");
            } else {
                processMeshBlock(kb, -1, "");
            }
        }
        // Resolve connectivity.
        for (const auto& pc : pending_) {
            Element& e = m.elements[pc.element];
            bool ok = true;
            e.conn.clear();
            for (long long lab : pc.labels) {
                const std::int32_t ni = m.findNode({pc.instance, lab});
                if (ni < 0) {
                    ok = false;
                    error(pc.fileIndex, pc.line,
                          "element " + std::to_string(e.label) + " references undefined node " + std::to_string(lab));
                    continue;
                }
                e.conn.push_back(ni);
            }
            if (!ok) e.info.shape = ElementShape::Unknown;
        }
        // Sort/dedupe sets (Abaqus sets are sorted, unique).
        for (auto& [k, v] : d.nsets_) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        for (auto& [k, v] : d.elsets_) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        // Spatial dimension.
        int dim = 0;
        for (const auto& e : m.elements) {
            const int ed = elementSpatialDim(e);
            dim = std::max(dim, ed);
        }
        if (dim == 0) {
            dim = 2;
            for (std::int32_t i = 0; i < m.nodeCount(); ++i)
                if (m.xyz[3 * i + 2] != 0.0) dim = 3;
        }
        m.spatialDim = dim;
        d.nodeDefs_.resize(m.nodeCount());
    }

    int elementSpatialDim(const Element& e) const {
        const std::string& t = e.type;
        switch (e.info.family) {
            case ElementFamily::Continuum2D:
            case ElementFamily::Axisymmetric: return 2;
            case ElementFamily::Continuum3D:
            case ElementFamily::Shell:
            case ElementFamily::Membrane: return t.rfind("SAX", 0) == 0 || t.rfind("MAX", 0) == 0 ? 2 : 3;
            case ElementFamily::User: {
                auto it = userElementCoords_.find(t);
                return it == userElementCoords_.end() ? 0 : (it->second >= 3 ? 3 : 2);
            }
            default: break;
        }
        if (t.find("2D") != std::string::npos || t.rfind("B2", 0) == 0 || t.rfind("PIPE2", 0) == 0 ||
            t.rfind("RAX", 0) == 0)
            return 2;
        if (t.find("3D") != std::string::npos || t.rfind("B3", 0) == 0 || t.rfind("PIPE3", 0) == 0 ||
            t.rfind("ELBOW", 0) == 0)
            return 3;
        return 0;
    }

    void applyTransform(std::int32_t inst, const std::array<double, 3>& tr,
                        const std::vector<std::array<double, 7>>& rots) {
        Mesh& m = d.mesh_;
        for (std::int32_t i = 0; i < m.nodeCount(); ++i) {
            if (m.nodeKeys[i].instance != inst) continue;
            std::array<double, 3> x = m.coord(i);
            for (int k = 0; k < 3; ++k) x[k] += tr[k];
            for (const auto& r : rots) x = rotateAbout(x, {r[0], r[1], r[2]}, {r[3], r[4], r[5]}, r[6]);
            for (int k = 0; k < 3; ++k) m.xyz[3 * i + k] = x[k];
        }
    }

    void addToSet(std::map<std::string, std::vector<std::int32_t>>& sets, const std::string& key, std::int32_t idx) {
        sets[key].push_back(idx);
    }

    /// Resolve a set member token to node/element indices.
    void resolveMember(const KeywordBlock& kb, const std::string& tok, std::int32_t inst, const std::string& prefix,
                       bool nodes, std::vector<std::int32_t>& out) {
        if (tok.empty()) return;
        if (auto lab = parseInteger(tok)) {
            const std::int32_t i = nodes ? d.mesh_.findNode({inst, *lab}) : findElement(inst, *lab);
            if (i >= 0)
                out.push_back(i);
            else
                warnOnce(kb, std::string(nodes ? "node" : "element") + " label " + std::to_string(*lab) +
                                 " in set definition is not defined (further warnings for this card suppressed)");
            return;
        }
        const std::string u = upper(unquote(tok));
        // "Instance.label"
        const std::size_t dot = u.rfind('.');
        if (dot != std::string::npos) {
            const std::int32_t ii = instanceIndex(u.substr(0, dot));
            const auto lab = parseInteger(u.substr(dot + 1));
            if (ii >= 0 && lab) {
                const std::int32_t i = nodes ? d.mesh_.findNode({ii, *lab}) : findElement(ii, *lab);
                if (i >= 0) out.push_back(i);
                return;
            }
        }
        const auto& sets = nodes ? d.nsets_ : d.elsets_;
        for (const std::string& key : {prefix + u, u}) {
            auto it = sets.find(key);
            if (it != sets.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
                return;
            }
        }
        warnOnce(kb, std::string(nodes ? "node" : "element") + " set member '" + tok + "' could not be resolved");
    }

    std::int32_t findElement(std::int32_t inst, long long label) const {
        auto it = elementIndex_.find({inst, label});
        return it == elementIndex_.end() ? -1 : it->second;
    }

    void processMeshBlock(const KeywordBlock& kb, std::int32_t inst, const std::string& prefix) {
        Mesh& m = d.mesh_;
        const std::string& kw = kb.keyword;
        if (kw == "*NODE") {
            if (kb.hasParam("SYSTEM"))
                warnOnce(kb, "*NODE, SYSTEM= (non-rectangular coordinates) is not converted; coordinates used as written");
            const std::string nset = upper(unquote(kb.param("NSET").value_or("")));
            for (const auto& ln : kb.data) {
                const auto f = splitFields(ln.text);
                if (f.empty()) continue;
                const auto lab = parseInteger(f[0]);
                if (!lab) {
                    warn(ln.fileIndex, ln.lineNumber, "invalid node label '" + f[0] + "'");
                    continue;
                }
                std::array<double, 3> x{0, 0, 0};
                for (std::size_t k = 1; k < f.size() && k <= 3; ++k) {
                    auto v = parseNumber(f[k]);
                    if (!v && !f[k].empty())
                        warn(ln.fileIndex, ln.lineNumber, "non-numeric node coordinate '" + f[k] + "'");
                    x[k - 1] = v.value_or(0.0);
                }
                const std::int32_t idx = m.addNode({inst, *lab}, x);
                if (static_cast<std::int32_t>(d.nodeDefs_.size()) <= idx) d.nodeDefs_.resize(idx + 1);
                d.nodeDefs_[idx] = {ln.fileIndex, ln.lineNumber};
                if (!nset.empty()) addToSet(d.nsets_, prefix + nset, idx);
            }
        } else if (kw == "*ELEMENT") {
            const std::string type = upper(unquote(kb.param("TYPE").value_or("")));
            if (type.empty()) warnOnce(kb, "*ELEMENT without TYPE=");
            const std::string elset = upper(unquote(kb.param("ELSET").value_or("")));
            int expected = 0;
            auto ue = userElementNodes_.find(type);
            if (ue != userElementNodes_.end()) {
                expected = ue->second;
            } else {
                const ElementTypeInfo ti = classifyElementType(type, 0);
                if (ti.family != ElementFamily::Unknown && ti.family != ElementFamily::User)
                    expected = shapeNodeCount(ti.shape);
            }
            std::vector<long long> labels;
            std::optional<long long> elabel;
            int startFile = 0, startLine = 0;
            auto flush = [&]() {
                if (!elabel) return;
                Element e;
                e.label = *elabel;
                e.instance = inst;
                e.type = type;
                e.info = classifyElementType(type, static_cast<int>(labels.size()));
                if (e.info.family == ElementFamily::User) {
                    auto c = userElementCoords_.find(type);
                    if (c != userElementCoords_.end() && c->second == 2 && labels.size() == 8)
                        e.info.shape = ElementShape::Quad8;
                    if (c != userElementCoords_.end() && c->second == 3 && labels.size() == 4)
                        e.info.shape = ElementShape::Tet4;
                    switch (e.info.shape) {
                        case ElementShape::Quad8: e.info.topoDim = 2; e.info.cornerNodes = 4; break;
                        case ElementShape::Tet4: e.info.topoDim = 3; e.info.cornerNodes = 4; break;
                        default: break;
                    }
                }
                if (expected > 0 && static_cast<int>(labels.size()) != expected)
                    warn(startFile, startLine,
                         "element " + std::to_string(e.label) + " of type " + type + " has " +
                             std::to_string(labels.size()) + " nodes, expected " + std::to_string(expected));
                const std::int32_t idx = m.elementCount();
                auto [it, inserted] = elementIndex_.emplace(NodeKey{inst, e.label}, idx);
                if (!inserted) warn(startFile, startLine, "element " + std::to_string(e.label) + " redefined");
                it->second = idx;
                m.elements.push_back(std::move(e));
                pending_.push_back({idx, inst, labels, startFile, startLine});
                if (!elset.empty()) addToSet(d.elsets_, prefix + elset, idx);
                labels.clear();
                elabel.reset();
            };
            for (const auto& ln : kb.data) {
                const auto f = splitFields(ln.text);
                const std::string t = trim(ln.text);
                const bool trailingComma = !t.empty() && t.back() == ',';
                if (f.empty()) continue;
                std::size_t k0 = 0;
                if (!elabel) {
                    elabel = parseInteger(f[0]);
                    if (!elabel) {
                        warn(ln.fileIndex, ln.lineNumber, "invalid element label '" + f[0] + "'");
                        continue;
                    }
                    startFile = ln.fileIndex;
                    startLine = ln.lineNumber;
                    k0 = 1;
                }
                for (std::size_t k = k0; k < f.size(); ++k) {
                    if (f[k].empty()) continue;
                    auto v = parseInteger(f[k]);
                    if (!v) {
                        warn(ln.fileIndex, ln.lineNumber, "invalid node label '" + f[k] + "' in element connectivity");
                        continue;
                    }
                    labels.push_back(*v);
                }
                const bool more = expected > 0 ? static_cast<int>(labels.size()) < expected : trailingComma;
                if (!more) flush();
            }
            if (elabel) flush();
        } else if (kw == "*NSET" || kw == "*ELSET") {
            const bool nodes = kw == "*NSET";
            const std::string name = upper(unquote(kb.param(nodes ? "NSET" : "ELSET").value_or("")));
            if (name.empty()) {
                warnOnce(kb, kw + " without a set name");
                return;
            }
            std::int32_t scope = inst;
            std::string scopePrefix = prefix;
            if (auto in = kb.param("INSTANCE")) {
                scope = instanceIndex(upper(unquote(*in)));
                if (scope < 0) {
                    warnOnce(kb, kw + ": unknown INSTANCE=" + *in);
                    return;
                }
                scopePrefix = upper(unquote(*in)) + ".";
            }
            std::vector<std::int32_t> members;
            if (nodes && kb.hasParam("ELSET")) {
                std::vector<std::int32_t> els;
                resolveMember(kb, unquote(*kb.param("ELSET")), scope, scopePrefix, false, els);
                // Connectivity is resolved at the end; use the pending labels (pending_[e] belongs to element e).
                for (std::int32_t e : els) {
                    const PendingConn& pc = pending_[e];
                    for (long long lab : pc.labels) {
                        const std::int32_t ni = m.findNode({pc.instance, lab});
                        if (ni >= 0) members.push_back(ni);
                    }
                }
            }
            const bool generate = kb.hasParam("GENERATE");
            for (const auto& ln : kb.data) {
                const auto f = splitFields(ln.text);
                if (generate) {
                    if (f.size() < 2) {
                        warn(ln.fileIndex, ln.lineNumber, "GENERATE line needs first, last[, increment]");
                        continue;
                    }
                    const auto a = parseInteger(f[0]), b = parseInteger(f[1]);
                    const auto inc = f.size() > 2 && !f[2].empty() ? parseInteger(f[2]) : std::optional<long long>(1);
                    if (!a || !b || !inc || *inc <= 0 || *b < *a) {
                        warn(ln.fileIndex, ln.lineNumber, "invalid GENERATE range");
                        continue;
                    }
                    for (long long lab = *a; lab <= *b; lab += *inc)
                        resolveMember(kb, std::to_string(lab), scope, scopePrefix, nodes, members);
                } else {
                    for (const auto& tok : f) resolveMember(kb, tok, scope, scopePrefix, nodes, members);
                }
            }
            // Assembly-level sets defined with INSTANCE= are assembly sets (key without prefix).
            const std::string key = prefix + name;
            auto& dst = nodes ? d.nsets_[key] : d.elsets_[key];
            dst.insert(dst.end(), members.begin(), members.end());
        }
    }
};

// ---------------------------------------------------------------------------------------------------------
// Deck
// ---------------------------------------------------------------------------------------------------------
Deck Deck::load(const std::string& path) {
    bool ok = false;
    const std::string text = readFileBytes(path, ok);
    if (!ok) throw std::runtime_error("cannot open INP file: " + path);
    DeckBuilder b;
    b.loadRoot(text, path);
    return std::move(b.d);
}

Deck Deck::fromString(const std::string& text, const std::string& virtualPath) {
    DeckBuilder b;
    b.loadRoot(text, virtualPath);
    return std::move(b.d);
}

std::vector<std::string> Deck::missingIncludes() const { return missing_; }

bool Deck::usesUserMaterial() const {
    return std::any_of(blocks_.begin(), blocks_.end(), [](const KeywordBlock& b) { return b.keyword == "*USER MATERIAL"; });
}

bool Deck::usesUserElement() const {
    return std::any_of(blocks_.begin(), blocks_.end(), [](const KeywordBlock& b) { return b.keyword == "*USER ELEMENT"; });
}

std::string Deck::fileText(int fileIndex) const {
    if (fileIndex < 0 || fileIndex >= static_cast<int>(files_.size()))
        throw std::out_of_range("Deck::fileText: bad file index");
    const SourceFile& f = files_[fileIndex];
    std::string out;
    std::size_t total = 0;
    for (const auto& l : f.lines) total += l.size() + f.lineEnding.size();
    out.reserve(total);
    for (std::size_t i = 0; i < f.lines.size(); ++i) {
        out += f.lines[i];
        if (i + 1 < f.lines.size() || f.finalNewline) out += f.lineEnding;
    }
    return out;
}

LineLoc Deck::nodeDefinition(std::int32_t node) const {
    if (node < 0 || node >= static_cast<std::int32_t>(nodeDefs_.size())) return {};
    return nodeDefs_[node];
}

std::vector<std::int32_t> Deck::resolveNodes(const std::string& targetToken, const std::string& scopeInstance) const {
    std::vector<std::int32_t> out;
    const std::string tok = trim(unquote(targetToken));
    if (tok.empty()) return out;
    const std::string scope = upper(trim(scopeInstance));
    std::int32_t scopeIdx = -1;
    if (!scope.empty())
        for (std::size_t i = 0; i < mesh_.instanceNames.size(); ++i)
            if (iequals(mesh_.instanceNames[i], scope)) scopeIdx = static_cast<std::int32_t>(i);
    if (auto lab = parseInteger(tok)) {
        const std::int32_t i = mesh_.findNode({scopeIdx, *lab});
        if (i >= 0) out.push_back(i);
        return out;
    }
    const std::string u = upper(tok);
    const std::size_t dot = u.rfind('.');
    if (dot != std::string::npos) {
        const std::string inst = u.substr(0, dot);
        const auto lab = parseInteger(u.substr(dot + 1));
        if (lab)
            for (std::size_t i = 0; i < mesh_.instanceNames.size(); ++i)
                if (iequals(mesh_.instanceNames[i], inst)) {
                    const std::int32_t ni = mesh_.findNode({static_cast<std::int32_t>(i), *lab});
                    if (ni >= 0) out.push_back(ni);
                    return out;
                }
    }
    if (!scope.empty()) {
        auto it = nsets_.find(scope + "." + u);
        if (it != nsets_.end()) return it->second;
    }
    auto it = nsets_.find(u);
    if (it != nsets_.end()) return it->second;
    return out;
}

namespace {
struct NamedBc {
    const char* name;
    std::vector<std::pair<int, int>> ranges;
};
const std::vector<NamedBc>& namedBoundaryTypes() {
    static const std::vector<NamedBc> t = {
        {"ENCASTRE", {{1, 6}}},          {"PINNED", {{1, 3}}},
        {"XSYMM", {{1, 1}, {5, 6}}},     {"YSYMM", {{2, 2}, {4, 4}, {6, 6}}},
        {"ZSYMM", {{3, 5}}},             {"XASYMM", {{2, 4}}},
        {"YASYMM", {{1, 1}, {3, 3}, {5, 5}}}, {"ZASYMM", {{1, 2}, {6, 6}}},
    };
    return t;
}
}  // namespace

std::vector<BoundaryEntry> Deck::boundaries() const {
    std::vector<BoundaryEntry> out;
    for (int bi = 0; bi < static_cast<int>(blocks_.size()); ++bi) {
        const KeywordBlock& kb = blocks_[bi];
        if (kb.keyword != "*BOUNDARY") continue;
        if (!kb.partName.empty() && kb.instanceName.empty()) continue;  // part-level BCs are not flattened
        const std::string op = upper(kb.param("OP").value_or("MOD"));
        const std::string type = upper(kb.param("TYPE").value_or("DISPLACEMENT"));
        const std::string amp = unquote(kb.param("AMPLITUDE").value_or(""));
        for (const auto& ln : kb.data) {
            const auto spans = fieldSpans(ln.text);
            auto field = [&](std::size_t k) -> std::string {
                return k < spans.size() ? ln.text.substr(spans[k].begin, spans[k].end - spans[k].begin) : std::string();
            };
            const std::string target = field(0);
            if (target.empty()) continue;
            BoundaryEntry proto;
            proto.blockIndex = bi;
            proto.stepIndex = kb.stepIndex;
            proto.target = target;
            proto.amplitude = amp;
            proto.op = op;
            proto.type = type;
            proto.nodes = resolveNodes(target, kb.instanceName);
            proto.valueRef.file = files_[ln.fileIndex].path;
            proto.valueRef.line = ln.lineNumber;
            proto.valueRef.keyword = kb.keyword;
            proto.valueRef.field = -1;
            const std::string f1 = upper(field(1));
            if (f1.empty()) continue;
            if (auto first = parseInteger(f1)) {
                proto.firstDof = static_cast<int>(*first);
                const std::string f2 = field(2);
                auto last = f2.empty() ? std::optional<long long>() : parseInteger(f2);
                proto.lastDof = last ? static_cast<int>(*last) : proto.firstDof;
                const std::string f3 = field(3);
                if (!f3.empty()) {
                    if (auto mag = parseNumber(f3)) {
                        proto.magnitude = *mag;
                        proto.valueRef.field = 3;
                    }
                }
                out.push_back(proto);
            } else {
                bool found = false;
                for (const auto& nb : namedBoundaryTypes())
                    if (f1 == nb.name) {
                        found = true;
                        for (auto [a, b] : nb.ranges) {
                            BoundaryEntry e = proto;
                            e.firstDof = a;
                            e.lastDof = b;
                            out.push_back(e);
                        }
                    }
                (void)found;
            }
        }
    }
    return out;
}

}  // namespace abss::inp
