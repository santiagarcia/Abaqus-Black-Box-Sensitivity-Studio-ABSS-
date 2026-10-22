#include "abaqus_replica.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

namespace abss::abq {

namespace {

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : t) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            f.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    f.push_back(trim(cur));
    return f;  // a trailing comma leaves an empty last field (kept: it marks a continued data line)
}

std::string join(const std::vector<std::string>& f) {
    std::string s;
    for (size_t i = 0; i < f.size(); ++i) s += (i ? ", " : "") + f[i];
    return s;
}

bool isInteger(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool isKeyword(const std::string& line) { return !line.empty() && line[0] == '*' && (line.size() < 2 || line[1] != '*'); }
bool isComment(const std::string& line) { return line.size() >= 2 && line[0] == '*' && line[1] == '*'; }

struct Card {
    std::string name;                                          ///< "*SOLID SECTION"
    std::vector<std::pair<std::string, std::string>> params;   ///< key (as written), value (as written, may be empty)
    std::vector<bool> hasValue;
};

Card parseCard(const std::string& text) {
    Card c;
    const auto f = fields(text);
    std::string n = upper(f[0]);
    std::string norm;
    for (char ch : n) {
        if (ch == ' ' && (!norm.empty() && norm.back() == ' ')) continue;
        norm += ch;
    }
    c.name = trim(norm);
    for (size_t i = 1; i < f.size(); ++i) {
        if (f[i].empty()) continue;
        const auto eq = f[i].find('=');
        if (eq == std::string::npos) {
            c.params.emplace_back(trim(f[i]), "");
            c.hasValue.push_back(false);
        } else {
            c.params.emplace_back(trim(f[i].substr(0, eq)), trim(f[i].substr(eq + 1)));
            c.hasValue.push_back(true);
        }
    }
    return c;
}

std::string writeCard(const Card& c, const std::string& originalName) {
    std::string s = originalName;
    for (size_t i = 0; i < c.params.size(); ++i)
        s += ", " + c.params[i].first + (c.hasValue[i] ? "=" + c.params[i].second : "");
    return s;
}

const std::string* paramOf(const Card& c, const char* key) {
    for (const auto& [k, v] : c.params)
        if (upper(k) == key) return &v;
    return nullptr;
}

void renameParams(Card& c, int copy, std::initializer_list<const char*> keys) {
    for (auto& [k, v] : c.params)
        for (const char* key : keys)
            if (upper(k) == key && !v.empty()) v = replicaName(v, copy);
}

[[noreturn]] void unsupported(const std::string& kw, const std::string& why = "") {
    throw std::runtime_error("Replicated branches do not support " + kw + (why.empty() ? "" : " (" + why + ")") +
                             "; branches run as separate Abaqus jobs instead.");
}

// One label-or-name reference: integer labels move by the offset, names get the copy suffix.
std::string ref(const std::string& f, int copy, std::int64_t off) {
    if (f.empty()) return f;
    if (isInteger(f)) return std::to_string(std::stoll(f) + copy * off);
    return replicaName(f, copy);
}

// Keyword blocks of a text: [card line(s) joined, data lines]. Comments dropped.
struct Block {
    std::string card;
    std::vector<std::string> data;
};

std::vector<Block> blocksOf(const std::string& text, std::vector<std::string>* preamble = nullptr) {
    std::vector<Block> out;
    const auto lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];
        if (isComment(l) || trim(l).empty()) continue;
        if (isKeyword(l)) {
            Block b;
            b.card = l;
            // keyword continuation: a card line ending with ',' continues on the next line
            while (!trim(b.card).empty() && trim(b.card).back() == ',' && i + 1 < lines.size()) b.card = trim(b.card) + " " + lines[++i];
            out.push_back(std::move(b));
        } else if (out.empty()) {
            if (preamble) preamble->push_back(l);
        } else {
            out.back().data.push_back(l);
        }
    }
    return out;
}

const std::set<std::string>& materialOptions() {
    static const std::set<std::string> s = {
        "*ELASTIC", "*PLASTIC", "*DENSITY", "*EXPANSION", "*USER MATERIAL", "*DEPVAR", "*HYPERELASTIC", "*HYPERFOAM",
        "*VISCOELASTIC", "*CREEP", "*DAMPING", "*CONDUCTIVITY", "*SPECIFIC HEAT", "*LATENT HEAT", "*USER OUTPUT VARIABLES",
        "*CYCLIC HARDENING", "*RATE DEPENDENT", "*POTENTIAL", "*VISCOUS", "*NO COMPRESSION", "*NO TENSION",
        "*ANNEAL TEMPERATURE", "*TEST DATA", "*UNIAXIAL TEST DATA", "*BIAXIAL TEST DATA", "*PLANAR TEST DATA",
        "*VOLUMETRIC TEST DATA", "*MULLINS EFFECT", "*TRS", "*SHEAR TEST DATA", "*HYPOELASTIC", "*POROUS ELASTIC"};
    return s;
}

}  // namespace

ReplicaNumbering replicaNumbering(std::int64_t maxNode, std::int64_t maxElem) {
    auto up = [](std::int64_t m) {
        std::int64_t p = 10;
        while (p <= m) p *= 10;
        return p;
    };
    return {up(maxNode), up(maxElem)};
}

std::string replicaName(const std::string& name, int copy) {
    if (copy == 0 || name.empty()) return name;
    const std::string suffix = "_ABSSC" + std::to_string(copy);
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') return name.substr(0, name.size() - 1) + suffix + "\"";
    return name + suffix;
}

std::string replicateModelData(const std::string& modelData, int copy, const ReplicaNumbering& n) {
    if (copy == 0) return modelData;
    std::string out;
    const auto blocks = blocksOf(modelData);
    for (const auto& b : blocks) {
        Card c = parseCard(b.card);
        const std::string orig = trim(fields(b.card)[0]);
        const std::string& k = c.name;
        // model-wide cards: written once, by copy 0
        if (k == "*HEADING" || k == "*FILE FORMAT" || k == "*PREPRINT" || k == "*PHYSICAL CONSTANTS") continue;
        if (k == "*INITIAL CONDITIONS") {
            const auto* t = paramOf(c, "TYPE");
            const std::string type = t ? upper(*t) : "";
            if (type == "SOLUTION" && paramOf(c, "USER")) continue;  // user initialisation applies to every element
            if (type != "TEMPERATURE" && type != "VELOCITY") unsupported(k, "TYPE=" + type);
            if (paramOf(c, "INPUT")) unsupported(k, "INPUT=");
            out += writeCard(c, orig) + "\n";
            for (const auto& l : b.data) {
                auto f = fields(l);
                f[0] = ref(f[0], copy, n.nodeOffset);
                out += join(f) + "\n";
            }
            continue;
        }
        std::vector<std::string> data = b.data;
        if (k == "*NODE") {
            if (paramOf(c, "INPUT")) unsupported(k, "INPUT=");
            renameParams(c, copy, {"NSET"});
            for (auto& l : data) {
                auto f = fields(l);
                f[0] = ref(f[0], copy, n.nodeOffset);
                l = join(f);
            }
        } else if (k == "*ELEMENT") {
            if (paramOf(c, "INPUT")) unsupported(k, "INPUT=");
            renameParams(c, copy, {"ELSET"});
            bool continued = false;
            for (auto& l : data) {
                auto f = fields(l);
                for (size_t i = 0; i < f.size(); ++i) {
                    if (f[i].empty()) continue;
                    f[i] = ref(f[i], copy, (i == 0 && !continued) ? n.elemOffset : n.nodeOffset);
                }
                continued = !f.empty() && f.back().empty();
                l = join(f);
            }
        } else if (k == "*NSET" || k == "*ELSET") {
            if (paramOf(c, "INSTANCE")) unsupported(k, "INSTANCE=");
            const bool nset = k == "*NSET";
            renameParams(c, copy, {"NSET", "ELSET"});
            const bool generate = paramOf(c, "GENERATE") != nullptr;
            // *NSET, ELSET=e takes the nodes of an element set: its data (if any) are still node labels/sets.
            const std::int64_t off = nset ? n.nodeOffset : n.elemOffset;
            for (auto& l : data) {
                auto f = fields(l);
                if (generate) {
                    for (size_t i = 0; i < f.size() && i < 2; ++i)
                        if (isInteger(f[i])) f[i] = std::to_string(std::stoll(f[i]) + copy * off);
                } else {
                    for (auto& x : f) x = ref(x, copy, off);
                }
                l = join(f);
            }
        } else if (k == "*SURFACE") {
            const auto* t = paramOf(c, "TYPE");
            const std::string type = t ? upper(*t) : "ELEMENT";
            if (type != "ELEMENT" && type != "NODE") unsupported(k, "TYPE=" + type);
            renameParams(c, copy, {"NAME"});
            for (auto& l : data) {
                auto f = fields(l);
                f[0] = ref(f[0], copy, type == "NODE" ? n.nodeOffset : n.elemOffset);
                l = join(f);
            }
        } else if (k == "*SOLID SECTION" || k == "*SHELL SECTION" || k == "*MEMBRANE SECTION" || k == "*BEAM SECTION" ||
                   k == "*BEAM GENERAL SECTION" || k == "*SHELL GENERAL SECTION" || k == "*COHESIVE SECTION") {
            if (paramOf(c, "COMPOSITE")) unsupported(k, "COMPOSITE");
            renameParams(c, copy, {"ELSET", "MATERIAL", "ORIENTATION", "CONTROLS"});
        } else if (k == "*MATERIAL" || k == "*ORIENTATION" || k == "*AMPLITUDE" || k == "*SECTION CONTROLS") {
            renameParams(c, copy, {"NAME"});
        } else if (materialOptions().count(k)) {
            // material options carry data only
        } else if (k == "*TRANSFORM") {
            renameParams(c, copy, {"NSET"});
        } else if (k == "*MASS" || k == "*ROTARY INERTIA") {
            renameParams(c, copy, {"ELSET"});
        } else if (k == "*BOUNDARY") {
            renameParams(c, copy, {"AMPLITUDE"});
            for (auto& l : data) {
                auto f = fields(l);
                f[0] = ref(f[0], copy, n.nodeOffset);
                l = join(f);
            }
        } else {
            unsupported(k);
        }
        out += writeCard(c, orig) + "\n";
        for (const auto& l : data) out += l + "\n";
    }
    return out;
}

std::string replicateStepData(const std::string& stepText, int copy, const ReplicaNumbering& n) {
    if (copy == 0) return stepText;
    std::string out;
    for (const auto& b : blocksOf(stepText)) {
        Card c = parseCard(b.card);
        const std::string& k = c.name;
        std::int64_t off = -1;
        bool surface = false;
        if (k == "*BOUNDARY" || k == "*CLOAD" || k == "*TEMPERATURE") off = n.nodeOffset;
        else if (k == "*DLOAD") off = n.elemOffset;
        else if (k == "*DSLOAD") surface = true;
        if (off >= 0 || surface) {
            if (paramOf(c, "AMPLITUDE")) unsupported(k, "AMPLITUDE= in a branch step");
            out += b.card + "\n";
            for (const auto& l : b.data) {
                auto f = fields(l);
                // an empty target means "the whole model" (e.g. gravity): it would load every copy
                if (f[0].empty()) unsupported(k, "a load or BC without a target set");
                f[0] = surface ? replicaName(f[0], copy) : ref(f[0], copy, off);
                out += join(f) + "\n";
            }
        } else {
            out += b.card + "\n";
            for (const auto& l : b.data) out += l + "\n";
        }
    }
    return out;
}

std::string mergeCopySteps(const std::vector<std::string>& perCopy) {
    if (perCopy.empty()) return {};
    std::vector<std::vector<Block>> bl;
    for (const auto& t : perCopy) bl.push_back(blocksOf(t));
    for (size_t c = 1; c < bl.size(); ++c) {
        if (bl[c].size() != bl[0].size()) throw std::runtime_error("Replicated step texts differ in their keyword sequence");
        for (size_t i = 0; i < bl[0].size(); ++i)
            if (upper(trim(bl[c][i].card)) != upper(trim(bl[0][i].card)))
                throw std::runtime_error("Replicated step texts differ: '" + bl[0][i].card + "' vs '" + bl[c][i].card + "'");
    }
    std::string out;
    for (size_t i = 0; i < bl[0].size(); ++i) {
        const auto name = parseCard(bl[0][i].card).name;
        const bool perCopyData = name == "*BOUNDARY" || name == "*CLOAD" || name == "*DLOAD" || name == "*DSLOAD" ||
                                 name == "*TEMPERATURE";
        out += bl[0][i].card + "\n";
        if (perCopyData) {
            for (const auto& b : bl)
                for (const auto& l : b[i].data) out += l + "\n";
        } else {
            for (const auto& l : bl[0][i].data) out += l + "\n";
        }
    }
    return out;
}

FilIncrement extractCopy(const FilIncrement& all, int copy, const ReplicaNumbering& n) {
    FilIncrement r;
    r.step = all.step;
    r.increment = all.increment;
    r.totalTime = all.totalTime;
    r.stepTime = all.stepTime;
    auto nodes = [&](const std::map<std::int64_t, std::vector<double>>& src, std::map<std::int64_t, std::vector<double>>& dst) {
        for (const auto& [lab, v] : src)
            if (lab / n.nodeOffset == copy) dst[lab - copy * n.nodeOffset] = v;
    };
    nodes(all.U, r.U);
    nodes(all.RF, r.RF);
    nodes(all.CF, r.CF);
    nodes(all.V, r.V);
    nodes(all.A, r.A);
    for (const auto& p : all.points)
        if (p.element / n.elemOffset == copy) {
            auto q = p;
            q.element -= copy * n.elemOffset;
            r.points.push_back(std::move(q));
        }
    return r;
}

}  // namespace abss::abq
