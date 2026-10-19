// ABSS INP: token-level patching. Only the edited numeric tokens change; every other byte is preserved.
#include "abss/inp/deck.hpp"

#include "inp_util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace abss::inp {

using namespace detail;

std::string formatNumber(double v) {
    if (!std::isfinite(v)) throw std::invalid_argument("formatNumber: non-finite value");
    // Shortest representation that round-trips exactly (at most 17 significant digits), scientific form so
    // the token is unambiguous for Abaqus; mantissa always carries a decimal point, exponent letter 'E'.
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    if (ec != std::errc()) throw std::runtime_error("formatNumber: conversion failed");
    std::string s(buf, p);
    const std::size_t e = s.find('e');
    std::string mant = s.substr(0, e), expo = e == std::string::npos ? "+00" : s.substr(e + 1);
    if (mant.find('.') == std::string::npos) mant += ".0";
    return mant + "E" + expo;
}

std::vector<TokenEdit> editsForParameter(const ParameterInfo& p, double newValue) {
    if (!p.supported)
        throw std::invalid_argument("parameter '" + p.name + "' is not differentiable/editable: " + p.note);
    if (!std::isfinite(newValue)) throw std::invalid_argument("editsForParameter: non-finite value");
    std::vector<TokenEdit> out;
    out.reserve(p.targets.size());
    for (const auto& t : p.targets) {
        double v = newValue;
        switch (p.mode) {
            case EditMode::Replace: v = newValue; break;
            case EditMode::Scale:
                if (p.nominal == 0.0)
                    throw std::invalid_argument("editsForParameter: Scale mode with zero nominal for '" + p.name + "'");
                v = t.nominalToken * (newValue / p.nominal);
                break;
            case EditMode::Offset: v = t.nominalToken + (newValue - p.nominal) * t.weight; break;
        }
        out.push_back({t.ref, v});
    }
    return out;
}

namespace {

/// Index of the files whose absolute path matches `file` (an included file may be read more than once).
std::vector<int> filesMatching(const Deck& deck, const std::string& file) {
    std::vector<int> out;
    const std::string want = absoluteNormal(file);
    for (int i = 0; i < static_cast<int>(deck.files().size()); ++i)
        if (samePath(deck.files()[i].path, want)) out.push_back(i);
    return out;
}

struct FieldEdit {
    int field;
    double value;
    std::string keyword;
};

std::size_t contentEnd(const std::string& line) {
    std::size_t e = line.size();
    while (e > 0 && (line[e - 1] == '\r' || line[e - 1] == '\n')) --e;
    return e;
}

std::string editLine(const std::string& line, const std::vector<FieldEdit>& edits, const std::string& where) {
    {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '*') throw std::runtime_error("edit target is not a data line: " + where);
    }
    // *PARAMETER lines are "name = value": edit the right-hand side.
    if (!edits.empty() && edits.back().keyword == "*PARAMETER") {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) throw std::runtime_error("*PARAMETER edit target has no '=': " + where);
        std::size_t b = eq + 1, e = contentEnd(line);
        while (b < e && isSpace(line[b])) ++b;
        while (e > b && isSpace(line[e - 1])) --e;
        if (!parseNumber(line.substr(b, e - b)))
            throw std::runtime_error("*PARAMETER value is not a numeric literal: " + where);
        return line.substr(0, b) + formatNumber(edits.back().value) + line.substr(e);
    }
    std::map<int, double> byField;  // last edit of a field wins
    for (const auto& e : edits) byField[e.field] = e.value;
    const auto spans = fieldSpans(line);
    std::string out = line;
    for (auto it = byField.rbegin(); it != byField.rend(); ++it) {
        const int f = it->first;
        const std::string val = formatNumber(it->second);
        if (f < 0) throw std::runtime_error("negative field index: " + where);
        if (f < static_cast<int>(spans.size())) {
            const auto sp = spans[f];
            const std::string tok = line.substr(sp.begin, sp.end - sp.begin);
            if (!tok.empty() && !parseNumber(tok))
                throw std::runtime_error("field " + std::to_string(f) + " is not numeric ('" + tok + "'): " + where);
            std::string ins = val;
            if (tok.empty() && sp.begin > 0 && line[sp.begin - 1] == ',') ins = " " + val;
            out.replace(sp.begin, sp.end - sp.begin, ins);
        } else if (f == static_cast<int>(spans.size())) {
            const std::size_t e = contentEnd(out);
            out.insert(e, ", " + val);
        } else {
            throw std::runtime_error("field " + std::to_string(f) + " beyond end of line: " + where);
        }
    }
    return out;
}

/// Edited copies of all source files' lines.
std::vector<std::vector<std::string>> editedLines(const Deck& deck, const std::vector<TokenEdit>& edits) {
    std::vector<std::vector<std::string>> lines;
    lines.reserve(deck.files().size());
    for (const auto& f : deck.files()) lines.push_back(f.lines);
    std::map<std::pair<int, int>, std::vector<FieldEdit>> grouped;
    for (const auto& e : edits) {
        const auto fis = filesMatching(deck, e.ref.file);
        if (fis.empty()) throw std::runtime_error("edit refers to a file that is not part of the deck: " + e.ref.file);
        for (int fi : fis) {
            if (e.ref.line < 1 || e.ref.line > static_cast<int>(deck.files()[fi].lines.size()))
                throw std::runtime_error("edit line out of range: " + e.ref.file + ":" + std::to_string(e.ref.line));
            grouped[{fi, e.ref.line}].push_back({e.ref.field, e.value, normalizeName(e.ref.keyword)});
        }
    }
    for (const auto& [key, fe] : grouped) {
        const auto [fi, ln] = key;
        lines[fi][ln - 1] = editLine(deck.files()[fi].lines[ln - 1], fe,
                                     deck.files()[fi].path + ":" + std::to_string(ln));
    }
    return lines;
}

std::string joinLines(const SourceFile& f, const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || f.finalNewline) out += f.lineEnding;
    }
    return out;
}

/// Replace the value of the INPUT= parameter on the keyword card starting at (1-based) `line`.
bool rewriteInputParam(std::vector<std::string>& lines, int line, int cardLines, const std::string& newValue) {
    for (int k = 0; k < cardLines && line - 1 + k < static_cast<int>(lines.size()); ++k) {
        std::string& s = lines[line - 1 + k];
        const std::string u = upper(s);
        std::size_t pos = 0;
        while ((pos = u.find("INPUT", pos)) != std::string::npos) {
            std::size_t q = pos + 5;
            while (q < s.size() && isSpace(s[q])) ++q;
            const bool boundary = pos == 0 || s[pos - 1] == ',' || isSpace(s[pos - 1]) || s[pos - 1] == '*';
            if (q < s.size() && s[q] == '=' && boundary) {
                std::size_t vb = q + 1;
                while (vb < s.size() && isSpace(s[vb])) ++vb;
                std::size_t ve = vb;
                if (ve < s.size() && s[ve] == '"') {
                    ve = s.find('"', ve + 1);
                    ve = ve == std::string::npos ? contentEnd(s) : ve + 1;
                } else {
                    while (ve < contentEnd(s) && s[ve] != ',') ++ve;
                    while (ve > vb && isSpace(s[ve - 1])) --ve;
                }
                s.replace(vb, ve - vb, newValue);
                return true;
            }
            pos += 5;
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> applyEdits(const Deck& deck, const std::vector<TokenEdit>& edits) {
    const auto lines = editedLines(deck, edits);
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) out.push_back(joinLines(deck.files()[i], lines[i]));
    return out;
}

std::string writeEditedDeck(const Deck& deck, const std::vector<TokenEdit>& edits, const std::string& outDir,
                            const std::string& jobName) {
    namespace fs = std::filesystem;
    if (deck.files().empty()) throw std::runtime_error("writeEditedDeck: empty deck");
    if (jobName.empty()) throw std::invalid_argument("writeEditedDeck: empty job name");
    auto lines = editedLines(deck, edits);
    const auto& files = deck.files();
    const fs::path out = toPath(absoluteNormal(outDir));
    const fs::path rootDir = toPath(files[0].path).parent_path();
    std::vector<fs::path> dst(files.size());
    std::vector<bool> relocated(files.size(), false);
    dst[0] = out / toPath(jobName + ".inp");
    for (std::size_t i = 1; i < files.size(); ++i) {
        const fs::path src = toPath(files[i].path);
        const fs::path rel = src.lexically_relative(rootDir);
        const bool clean = !rel.empty() && !rel.is_absolute() && rel.begin() != rel.end() && *rel.begin() != "..";
        if (clean) {
            dst[i] = out / rel;
        } else {
            dst[i] = out / "abss_includes" / toPath(std::to_string(i) + "_" + fromPath(src.filename()));
            relocated[i] = true;
        }
    }
    for (std::size_t i = 0; i < files.size(); ++i)
        if (samePath(absoluteNormal(fromPath(dst[i])), files[i].path))
            throw std::runtime_error("writeEditedDeck: output would overwrite the original file " + files[i].path);
    // Point relocated includes at their copies (absolute path: unambiguous for any include resolution rule).
    for (std::size_t i = 1; i < files.size(); ++i) {
        if (!relocated[i]) continue;
        const int from = files[i].includedFromFile;
        if (from < 0) continue;
        int cardLines = 1;
        for (const auto& b : deck.blocks())
            if (b.fileIndex == from && b.line == files[i].includedFromLine) cardLines = b.keywordLineCount;
        std::string p = fromPath(dst[i].lexically_normal());
        if (p.find_first_of(" ,") != std::string::npos) p = "\"" + p + "\"";
        if (!rewriteInputParam(lines[from], files[i].includedFromLine, cardLines, p))
            throw std::runtime_error("writeEditedDeck: cannot locate INPUT= for " + files[i].path);
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::error_code ec;
        fs::create_directories(dst[i].parent_path(), ec);
        std::ofstream o(dst[i], std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("writeEditedDeck: cannot write " + fromPath(dst[i]));
        const std::string text = joinLines(files[i], lines[i]);
        o.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!o) throw std::runtime_error("writeEditedDeck: write failed for " + fromPath(dst[i]));
    }
    return fromPath(dst[0]);
}

std::string describeEdits(const Deck& deck, const std::vector<TokenEdit>& edits) {
    const auto lines = editedLines(deck, edits);
    std::ostringstream os;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& orig = deck.files()[i].lines;
        bool header = false;
        for (std::size_t l = 0; l < orig.size(); ++l) {
            if (orig[l] == lines[i][l]) continue;
            if (!header) {
                os << "--- " << deck.files()[i].path << "\n+++ " << deck.files()[i].path << " (edited)\n";
                header = true;
            }
            auto strip = [](std::string s) {
                while (!s.empty() && s.back() == '\r') s.pop_back();
                return s;
            };
            os << "@@ -" << (l + 1) << " +" << (l + 1) << " @@\n-" << strip(orig[l]) << "\n+" << strip(lines[i][l])
               << "\n";
        }
    }
    return os.str();
}

}  // namespace abss::inp
