#include "abaqus_files.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace abss::abq {

namespace {

using Token = std::variant<std::int64_t, double, std::string>;

double toDouble(std::string s) {
    // Abaqus writes non-finite values (from a user material) as NAN / INF text: keep them as such so the caller can
    // report which job and quantity went non-finite instead of failing on the number format.
    if (s.find("NAN") != std::string::npos || s.find("nan") != std::string::npos) return std::numeric_limits<double>::quiet_NaN();
    if (s.find("INF") != std::string::npos || s.find("inf") != std::string::npos)
        return s.find('-') != std::string::npos ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    for (char& c : s)
        if (c == 'D' || c == 'd') c = 'E';
    // classic-locale independent parse
    double v = 0;
    const char* b = s.data();
    while (*b == ' ') ++b;
    auto r = std::from_chars(b, s.data() + s.size(), v);
    if (r.ec != std::errc()) throw std::runtime_error("bad .fil float: " + s);
    return v;
}

std::vector<std::vector<Token>> filRecords(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open results file " + path);
    std::string raw, line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 80) line.append(80 - line.size(), ' ');
        raw += line;
    }
    // Token-by-token scan: a record starts with '*' at a token boundary. Character tokens ('A' + 8 characters) are
    // consumed whole, so text such as a heading containing '*' cannot split a record.
    std::vector<std::vector<Token>> recs;
    std::vector<Token> toks;
    size_t i = raw.find('*');
    while (i != std::string::npos && i < raw.size()) {
        const char c = raw[i];
        if (c == '*') {
            if (!toks.empty()) recs.push_back(std::move(toks));
            toks.clear();
            ++i;
        } else if (c == 'I') {
            if (i + 3 > raw.size()) break;
            const int w = std::stoi(raw.substr(i + 1, 2));
            toks.emplace_back(static_cast<std::int64_t>(std::stoll(raw.substr(i + 3, static_cast<size_t>(w)))));
            i += 3 + static_cast<size_t>(w);
        } else if (c == 'D') {
            toks.emplace_back(toDouble(raw.substr(i + 1, 22)));
            i += 23;
        } else if (c == 'A') {
            toks.emplace_back(raw.substr(i + 1, 8));
            i += 9;
        } else if (c == ' ') {
            ++i;
        } else {
            throw std::runtime_error("Unexpected token '" + std::string(1, c) + "' in results file " + path);
        }
    }
    if (!toks.empty()) recs.push_back(std::move(toks));
    return recs;
}

double num(const Token& t) {
    if (auto* d = std::get_if<double>(&t)) return *d;
    if (auto* i = std::get_if<std::int64_t>(&t)) return static_cast<double>(*i);
    throw std::runtime_error("expected number in .fil record");
}
std::int64_t inum(const Token& t) {
    if (auto* i = std::get_if<std::int64_t>(&t)) return *i;
    if (auto* d = std::get_if<double>(&t)) return static_cast<std::int64_t>(*d);
    throw std::runtime_error("expected integer in .fil record");
}

}  // namespace

std::vector<FilIncrement> readFil(const std::string& path) {
    // Record layout: [length, key, data...]. Keys: 2000 increment start, 1 element header, 11 S, 21 E, 89 LE, 22 PE,
    // 5 SDV, 101 U, 104 RF, 106 CF.
    static const std::map<std::int64_t, std::string> elementKeys = {{11, "S"}, {21, "E"}, {89, "LE"}, {22, "PE"}, {5, "SDV"}};
    std::vector<FilIncrement> incs;
    FilElementPoint* cur = nullptr;
    for (const auto& t : filRecords(path)) {
        if (t.size() < 2) continue;
        const auto key = inum(t[1]);
        if (key == 2000) {
            if (t.size() < 9) throw std::runtime_error("short increment header in " + path);
            FilIncrement inc;
            inc.totalTime = num(t[2]);
            inc.stepTime = num(t[3]);
            inc.step = static_cast<int>(inum(t[7]));
            inc.increment = static_cast<int>(inum(t[8]));
            incs.push_back(std::move(inc));
            cur = nullptr;
            continue;
        }
        if (incs.empty()) continue;
        auto& inc = incs.back();
        if (key == 101 || key == 102 || key == 103 || key == 104 || key == 106) {
            const auto node = inum(t[2]);
            std::vector<double> v;
            for (size_t k = 3; k < t.size(); ++k) v.push_back(num(t[k]));
            (key == 101 ? inc.U : key == 102 ? inc.V : key == 103 ? inc.A : key == 104 ? inc.RF : inc.CF)[node] = std::move(v);
        } else if (key == 1) {
            FilElementPoint p;
            p.element = inum(t[2]);
            p.ip = static_cast<int>(inum(t[3]));
            p.sectionPoint = t.size() > 4 ? static_cast<int>(inum(t[4])) : 0;
            inc.points.push_back(std::move(p));
            cur = &inc.points.back();
        } else if (auto it = elementKeys.find(key); it != elementKeys.end() && cur) {
            std::vector<double> v;
            // SDV records carry the variable values; PE carries components, yield flag... PEEQ last.
            // PE records carry an 8-character yield flag ("YES"/"NO") before PEEQ: keep numeric tokens only.
            for (size_t k = 2; k < t.size(); ++k)
                if (!std::holds_alternative<std::string>(t[k])) v.push_back(num(t[k]));
            // A long SDV list is written as several consecutive key-5 records for the same point: concatenate them
            // (keeping only the last record lost every variable but the last chunk).
            auto& dst = cur->values[it->second];
            if (key == 5 && !dst.empty()) dst.insert(dst.end(), v.begin(), v.end());
            else dst = std::move(v);
        }
    }
    return incs;
}

FilModel readFilModel(const std::string& path) {
    FilModel m;
    for (const auto& t : filRecords(path)) {
        if (t.size() < 3) continue;
        const auto key = inum(t[1]);
        if (key == 2000) break;  // model definition precedes the first increment
        if (key == 1901) {
            std::vector<double> x;
            for (size_t k = 3; k < t.size(); ++k) x.push_back(num(t[k]));
            m.nodes[inum(t[2])] = std::move(x);
        } else if (key == 1900) {
            std::string type;
            std::vector<std::int64_t> nodes;
            for (size_t k = 3; k < t.size(); ++k) {
                if (auto* s = std::get_if<std::string>(&t[k])) type += *s;
                else nodes.push_back(inum(t[k]));
            }
            while (!type.empty() && type.back() == ' ') type.pop_back();
            m.elements[inum(t[2])] = {type, std::move(nodes)};
        }
    }
    return m;
}

std::map<std::pair<int, int>, std::vector<EmoBlock>> readEmo(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open element matrix file " + path);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    static const std::regex hdr(R"(\*\*\s*ELEMENT NUMBER\s+(\d+)\s+STEP NUMBER\s+(\d+)\s+INCREMENT NUMBER\s+(\d+))");
    auto ints = [](const std::string& s) {
        std::vector<std::int64_t> v;
        std::string tok;
        std::stringstream ss(s);
        while (std::getline(ss, tok, ',')) {
            const auto b = tok.find_first_not_of(" *\t");
            if (b == std::string::npos) continue;
            v.push_back(std::stoll(tok.substr(b)));
        }
        return v;
    };
    std::map<std::pair<int, int>, std::vector<EmoBlock>> out;
    size_t i = 0;
    std::smatch m;
    while (i < lines.size()) {
        if (!std::regex_search(lines[i], m, hdr)) {
            ++i;
            continue;
        }
        EmoBlock b;
        b.element = std::stoll(m[1]);
        const int st = std::stoi(m[2]), inc = std::stoi(m[3]);
        while (i < lines.size() && lines[i].find("ELEMENT NODES") == std::string::npos) ++i;
        if (i + 2 >= lines.size()) throw std::runtime_error("truncated element matrix file " + path);
        b.nodes = ints(lines[i + 1]);
        for (auto d : ints(lines[i + 2])) b.dofs.push_back(static_cast<int>(d));
        i += 3;
        while (i < lines.size() && lines[i].rfind("*MATRIX", 0) != 0) ++i;
        ++i;
        const size_t n = b.nodes.size() * b.dofs.size();
        std::vector<double> vals;
        while (vals.size() < n * (n + 1) / 2 && i < lines.size()) {
            std::stringstream ss(lines[i++]);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (tok.find_first_not_of(" \t") != std::string::npos) vals.push_back(toDouble(tok));
        }
        if (vals.size() != n * (n + 1) / 2) throw std::runtime_error("element matrix size mismatch in " + path);
        b.K.assign(n * n, 0.0);
        size_t k = 0;
        for (size_t r = 0; r < n; ++r)
            for (size_t c = 0; c <= r; ++c) {
                b.K[r * n + c] = b.K[c * n + r] = vals[k++];
            }
        out[{st, inc}].push_back(std::move(b));
    }
    return out;
}

}  // namespace abss::abq
