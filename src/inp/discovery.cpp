// ABSS INP: discovery of differentiable parameter candidates (spec section 4).
//
// Rules: every candidate points at exact numeric tokens (file/line/field); semantics are only claimed where the
// Abaqus keyword documentation defines them (KnownSemantic); UMAT/UEL property arrays are named by position only
// (OpaqueIndexed, 1-based like the Fortran PROPS array).
#include "abss/inp/deck.hpp"

#include "inp_util.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace abss::inp {

using namespace detail;

namespace {

struct Tok {
    int fileIndex = 0;
    int line = 0;
    int field = 0;
    std::string text;
};

/// Rows of a keyword table: each row is the concatenated fields of `linesPerRow` consecutive data lines.
std::vector<std::vector<Tok>> rowsOf(const KeywordBlock& kb, int linesPerRow) {
    std::vector<std::vector<Tok>> rows;
    if (linesPerRow < 1) linesPerRow = 1;
    for (std::size_t li = 0; li < kb.data.size(); ++li) {
        if (li % linesPerRow == 0) rows.emplace_back();
        const auto& ln = kb.data[li];
        const auto f = splitFields(ln.text);
        for (std::size_t k = 0; k < f.size(); ++k)
            rows.back().push_back({ln.fileIndex, ln.lineNumber, static_cast<int>(k), f[k]});
    }
    return rows;
}

/// All fields of all data lines in order (for "eight values per line" lists such as PROPS).
std::vector<Tok> flatTokens(const KeywordBlock& kb) {
    std::vector<Tok> out;
    for (const auto& ln : kb.data) {
        const auto f = splitFields(ln.text);
        for (std::size_t k = 0; k < f.size(); ++k) out.push_back({ln.fileIndex, ln.lineNumber, static_cast<int>(k), f[k]});
    }
    return out;
}

int linesPerRowFor(int entriesPerRow) { return std::max(1, (entriesPerRow + 7) / 8); }

int intParam(const KeywordBlock& kb, const char* key, int dflt) {
    if (auto v = kb.param(key))
        if (auto i = parseInteger(*v)) return static_cast<int>(*i);
    return dflt;
}

std::string dofLabel(int dof, const std::string& type) {
    std::string base = "U", rot = "UR";
    if (type == "VELOCITY") base = "V", rot = "VR";
    if (type == "ACCELERATION") base = "A", rot = "AR";
    if (dof >= 1 && dof <= 3) return base + std::to_string(dof);
    if (dof >= 4 && dof <= 6) return rot + std::to_string(dof - 3);
    if (dof == 11) return "NT11";
    return "DOF" + std::to_string(dof);
}

const char* axisName(int d) { return d == 1 ? "X" : d == 2 ? "Y" : "Z"; }

class Discoverer {
public:
    explicit Discoverer(const Deck& deck) : deck_(deck) {
        for (const auto& s : deck.steps()) allSteps_.push_back(s.name);
    }

    std::vector<ParameterInfo> run() {
        materials();
        sections();
        loads();
        boundaryConditions();
        amplitudes();
        userElements();
        parameters();
        geometry();
        return std::move(out_);
    }

private:
    const Deck& deck_;
    std::vector<std::string> allSteps_;
    std::vector<ParameterInfo> out_;
    std::map<std::string, int> idCount_;

    SourceRef ref(const Tok& t, const std::string& keyword) const {
        SourceRef r;
        r.file = deck_.files()[t.fileIndex].path;
        r.line = t.line;
        r.field = t.field;
        r.keyword = keyword;
        return r;
    }

    std::string where(const Tok& t) const {
        return fromPath(toPath(deck_.files()[t.fileIndex].path).filename()) + ":" + std::to_string(t.line);
    }

    void push(ParameterInfo p) {
        const int n = ++idCount_[p.id];
        if (n > 1) p.id += "#" + std::to_string(n);
        out_.push_back(std::move(p));
    }

    /// Single-token parameter (Replace mode). Returns false if the token is not numeric.
    bool single(ParameterInfo p, const Tok& t, const std::string& keyword) {
        const auto v = parseNumber(t.text);
        if (!v) return false;
        p.nominal = *v;
        p.mode = EditMode::Replace;
        ParameterTarget tg;
        tg.ref = ref(t, keyword);
        tg.nominalToken = *v;
        p.targets.push_back(tg);
        if (!p.description.empty()) p.description += " ";
        p.description += "(" + where(t) + ", field " + std::to_string(t.field + 1) + ")";
        push(std::move(p));
        return true;
    }

    std::vector<std::string> stepsFrom(int stepIndex, const std::string& keyword) const {
        if (stepIndex < 0) return allSteps_;
        std::vector<std::string> s;
        for (int k = stepIndex; k < static_cast<int>(allSteps_.size()); ++k) {
            if (k > stepIndex) {
                // A later OP=NEW of the same keyword removes this definition.
                bool reset = false;
                for (const auto& b : deck_.blocks())
                    if (b.stepIndex == k && b.keyword == keyword && iequals(b.param("OP").value_or("MOD"), "NEW"))
                        reset = true;
                if (reset) break;
            }
            s.push_back(allSteps_[k]);
        }
        return s;
    }

    std::string stepName(int stepIndex) const {
        if (stepIndex < 0) return "initial";
        return stepIndex < static_cast<int>(allSteps_.size()) ? allSteps_[stepIndex] : "Step-" + std::to_string(stepIndex + 1);
    }

    // ------------------------------------------------------------------ materials
    struct CoefSpec {
        std::string name;
        bool positive = false;
        double lo = -1e300, hi = 1e300;
        std::string meaning;
    };

    void table(const KeywordBlock& kb, const std::string& mat, const std::string& idBase,
               const std::vector<CoefSpec>& specs, const std::string& what) {
        const int deps = intParam(kb, "DEPENDENCIES", 0);
        const int perRow = static_cast<int>(specs.size()) + 1 + deps;
        const auto rows = rowsOf(kb, linesPerRowFor(perRow));
        for (std::size_t r = 0; r < rows.size(); ++r) {
            std::string suffix;
            if (rows.size() > 1) {
                const std::size_t tIdx = specs.size();
                if (tIdx < rows[r].size() && parseNumber(rows[r][tIdx].text))
                    suffix = " [T=" + displayNumber(*parseNumber(rows[r][tIdx].text)) + "]";
                else
                    suffix = " [row " + std::to_string(r + 1) + "]";
            }
            for (std::size_t i = 0; i < specs.size() && i < rows[r].size(); ++i) {
                const CoefSpec& s = specs[i];
                ParameterInfo p;
                p.id = idBase + ":" + s.name + (rows.size() > 1 ? ":r" + std::to_string(r + 1) : "");
                p.name = mat + ": " + s.name + suffix;
                p.description = s.meaning + " of material " + mat + " (" + what + ")" +
                                (rows.size() > 1 ? ", table row " + std::to_string(r + 1) : "") + ".";
                p.category = ParamCategory::Material;
                p.confidence = Confidence::KnownSemantic;
                p.mustBePositive = s.positive;
                p.lowerBound = s.positive ? 0.0 : s.lo;
                p.upperBound = s.hi;
                p.association = "material " + mat;
                p.steps = allSteps_;
                single(std::move(p), rows[r][i], kb.keyword);
            }
        }
    }

    void elastic(const KeywordBlock& kb, const std::string& mat) {
        const std::string type = normalizeName(kb.param("TYPE").value_or("ISOTROPIC"));
        const std::string base = "mat:" + upper(mat) + ":elastic";
        auto mod = [](std::string n, std::string m) { return CoefSpec{std::move(n), true, 0.0, 1e300, std::move(m)}; };
        auto any = [](std::string n, std::string m) { return CoefSpec{std::move(n), false, -1e300, 1e300, std::move(m)}; };
        std::vector<CoefSpec> s;
        if (type == "ISOTROPIC") {
            s = {mod("E", "Young's modulus"), CoefSpec{"nu", false, -1.0, 0.5, "Poisson's ratio"}};
        } else if (type == "ENGINEERING CONSTANTS") {
            s = {mod("E1", "Young's modulus E1"), mod("E2", "Young's modulus E2"), mod("E3", "Young's modulus E3"),
                 any("nu12", "Poisson's ratio nu12"), any("nu13", "Poisson's ratio nu13"),
                 any("nu23", "Poisson's ratio nu23"), mod("G12", "shear modulus G12"),
                 mod("G13", "shear modulus G13"), mod("G23", "shear modulus G23")};
        } else if (type == "LAMINA") {
            s = {mod("E1", "Young's modulus E1"), mod("E2", "Young's modulus E2"), any("nu12", "Poisson's ratio nu12"),
                 mod("G12", "shear modulus G12"), mod("G13", "transverse shear modulus G13"),
                 mod("G23", "transverse shear modulus G23")};
        } else if (type == "ORTHOTROPIC") {
            for (const char* n : {"D1111", "D1122", "D2222", "D1133", "D2233", "D3333", "D1212", "D1313", "D2323"})
                s.push_back(any(n, std::string("elastic stiffness ") + n));
        } else if (type == "ANISOTROPIC") {
            for (const char* n : {"D1111", "D1122", "D2222", "D1133", "D2233", "D3333", "D1112", "D2212", "D3312",
                                  "D1212", "D1113", "D2213", "D3313", "D1213", "D1313", "D1123", "D2223", "D3323",
                                  "D1223", "D1323", "D2323"})
                s.push_back(any(n, std::string("elastic stiffness ") + n));
        } else if (type == "TRACTION") {
            s = {mod("Enn", "normal cohesive stiffness Enn"), mod("Ess", "shear cohesive stiffness Ess"),
                 mod("Ett", "shear cohesive stiffness Ett")};
        } else if (type == "SHEAR") {
            s = {mod("G", "shear modulus")};
        } else {
            return;  // other forms are not exposed
        }
        table(kb, mat, base, s, "*ELASTIC, TYPE=" + type);
    }

    void plastic(const KeywordBlock& kb, const std::string& mat) {
        const std::string hard = normalizeName(kb.param("HARDENING").value_or("ISOTROPIC"));
        const std::string base = "mat:" + upper(mat) + ":plastic";
        if (hard == "JOHNSON COOK") {
            table(kb, mat, base,
                  {CoefSpec{"A", true, 0, 1e300, "Johnson-Cook initial yield stress A"},
                   CoefSpec{"B", false, -1e300, 1e300, "Johnson-Cook hardening modulus B"},
                   CoefSpec{"n", false, -1e300, 1e300, "Johnson-Cook hardening exponent n"},
                   CoefSpec{"m", false, -1e300, 1e300, "Johnson-Cook thermal softening exponent m"},
                   CoefSpec{"T_melt", false, -1e300, 1e300, "Johnson-Cook melting temperature"},
                   CoefSpec{"T_transition", false, -1e300, 1e300, "Johnson-Cook transition temperature"}},
                  "*PLASTIC, HARDENING=JOHNSON COOK");
            return;
        }
        if (hard == "USER") return;
        // Tabular yield stress vs equivalent plastic strain (ISOTROPIC, KINEMATIC, COMBINED).
        for (std::size_t r = 0; r < kb.data.size(); ++r) {
            const auto& ln = kb.data[r];
            const auto f = splitFields(ln.text);
            if (f.empty()) continue;
            const auto ep = f.size() > 1 ? parseNumber(f[1]) : std::optional<double>(0.0);
            const auto temp = f.size() > 2 ? parseNumber(f[2]) : std::nullopt;
            std::string arg = "eps_p=" + (ep ? displayNumber(*ep) : (f.size() > 1 ? f[1] : std::string("0")));
            if (temp) arg += ", T=" + displayNumber(*temp);
            ParameterInfo p;
            p.id = base + ":sigma_y:" + std::to_string(r + 1);
            p.name = mat + ": sigma_y(" + arg + ")";
            p.description = "Yield stress of material " + mat + " at equivalent plastic strain " +
                            (ep ? displayNumber(*ep) : f.size() > 1 ? f[1] : std::string("0")) +
                            " (*PLASTIC, HARDENING=" + hard + ", table row " + std::to_string(r + 1) +
                            "). Perturbing one row changes the hardening curve locally; the response may be "
                            "non-smooth when the plastic state crosses a table breakpoint.";
            p.category = ParamCategory::Material;
            p.confidence = Confidence::KnownSemantic;
            p.mustBePositive = true;
            p.lowerBound = 0.0;
            p.association = "material " + mat;
            p.steps = allSteps_;
            single(std::move(p), Tok{ln.fileIndex, ln.lineNumber, 0, f[0]}, kb.keyword);
        }
    }

    void hyperelastic(const KeywordBlock& kb, const std::string& mat) {
        const std::string base = "mat:" + upper(mat) + ":hyperelastic";
        if (kb.hasParam("TEST DATA INPUT") || kb.hasParam("MARLOW") || kb.hasParam("USER")) return;
        const int n = intParam(kb, "N", 1);
        auto c = [](const std::string& name, const std::string& m) { return CoefSpec{name, false, -1e300, 1e300, m}; };
        auto pos = [](const std::string& name, const std::string& m) { return CoefSpec{name, true, 0.0, 1e300, m}; };
        auto dterms = [&](std::vector<CoefSpec>& s, int count) {
            for (int i = 1; i <= count; ++i) {
                const std::string nm = "D" + std::to_string(i);
                s.push_back(CoefSpec{nm, false, 0.0, 1e300, "compressibility coefficient " + nm});
            }
        };
        std::vector<CoefSpec> s;
        std::string model;
        if (kb.hasParam("NEO HOOKE")) {
            model = "NEO HOOKE";
            s = {pos("C10", "neo-Hookean coefficient C10")};
            dterms(s, 1);
        } else if (kb.hasParam("MOONEY-RIVLIN")) {
            model = "MOONEY-RIVLIN";
            s = {c("C10", "Mooney-Rivlin coefficient C10"), c("C01", "Mooney-Rivlin coefficient C01")};
            dterms(s, 1);
        } else if (kb.hasParam("YEOH")) {
            model = "YEOH";
            s = {c("C10", "Yeoh coefficient C10"), c("C20", "Yeoh coefficient C20"), c("C30", "Yeoh coefficient C30")};
            dterms(s, 3);
        } else if (kb.hasParam("REDUCED POLYNOMIAL")) {
            model = "REDUCED POLYNOMIAL";
            for (int i = 1; i <= n; ++i) s.push_back(c("C" + std::to_string(i) + "0", "reduced polynomial coefficient"));
            dterms(s, n);
        } else if (kb.hasParam("OGDEN")) {
            model = "OGDEN";
            for (int i = 1; i <= n; ++i) {
                s.push_back(c("mu" + std::to_string(i), "Ogden coefficient mu" + std::to_string(i)));
                s.push_back(c("alpha" + std::to_string(i), "Ogden exponent alpha" + std::to_string(i)));
            }
            dterms(s, n);
        } else if (kb.hasParam("ARRUDA-BOYCE")) {
            model = "ARRUDA-BOYCE";
            s = {pos("mu", "Arruda-Boyce shear modulus mu"), pos("lambda_m", "Arruda-Boyce locking stretch"),
                 CoefSpec{"D", false, 0.0, 1e300, "compressibility coefficient D"}};
        } else if (kb.hasParam("VAN DER WAALS")) {
            model = "VAN DER WAALS";
            s = {pos("mu", "Van der Waals shear modulus mu"), pos("lambda_m", "locking stretch lambda_m"),
                 c("a", "global interaction parameter a"), c("beta", "invariant mixture parameter beta"),
                 CoefSpec{"D", false, 0.0, 1e300, "compressibility coefficient D"}};
        } else {
            // POLYNOMIAL (default, N=1 when omitted).
            model = "POLYNOMIAL N=" + std::to_string(n);
            for (int deg = 1; deg <= n; ++deg)
                for (int i = deg; i >= 0; --i)
                    s.push_back(c("C" + std::to_string(i) + std::to_string(deg - i), "polynomial coefficient"));
            dterms(s, n);
        }
        for (auto& sp : s)
            if (sp.meaning == "polynomial coefficient" || sp.meaning == "reduced polynomial coefficient")
                sp.meaning += " " + sp.name;
        table(kb, mat, base, s, "*HYPERELASTIC, " + model);
    }

    void userMaterial(const KeywordBlock& kb, const std::string& mat) {
        const int declared = intParam(kb, "CONSTANTS", -1);
        const std::string type = normalizeName(kb.param("TYPE").value_or("MECHANICAL"));
        const std::string sub = type == "THERMAL" ? "UMATHT" : "UMAT";
        const auto toks = flatTokens(kb);
        int numericCount = 0;
        std::string note;
        if (declared >= 0 && static_cast<int>(toks.size()) != declared)
            note = "CONSTANTS=" + std::to_string(declared) + " but " + std::to_string(toks.size()) +
                   " values are given on the data lines.";
        for (std::size_t k = 0; k < toks.size(); ++k) {
            if (declared >= 0 && static_cast<int>(k) >= declared) break;
            ParameterInfo p;
            p.id = "umat:" + upper(mat) + ":props:" + std::to_string(k + 1);
            p.name = sub + " " + mat + ": PROPS[" + std::to_string(k + 1) + "]";
            p.description = "Constant number " + std::to_string(k + 1) + " (1-based PROPS(" + std::to_string(k + 1) +
                            ")) passed to the user subroutine " + sub + " for material " + mat +
                            ". Its physical meaning is defined only by the compiled subroutine and is unknown to ABSS.";
            p.category = ParamCategory::UserMaterial;
            p.confidence = Confidence::OpaqueIndexed;
            p.association = "material " + mat;
            p.steps = allSteps_;
            p.note = note;
            if (single(std::move(p), toks[k], kb.keyword)) ++numericCount;
        }
        (void)numericCount;
    }

    void depvar(const KeywordBlock& kb, const std::string& mat) {
        const auto toks = flatTokens(kb);
        if (toks.empty()) return;
        const auto v = parseNumber(toks[0].text);
        if (!v) return;
        ParameterInfo p;
        p.id = "umat:" + upper(mat) + ":depvar";
        p.name = mat + ": DEPVAR (number of SDVs)";
        p.description = "Number of solution-dependent state variables (SDV1..SDV" + displayNumber(*v) +
                        ") stored per material point for material " + mat + ". Information only.";
        p.category = ParamCategory::UserMaterial;
        p.confidence = Confidence::KnownSemantic;
        p.nominal = *v;
        p.association = "material " + mat;
        p.steps = allSteps_;
        p.supported = false;
        p.note = "Integer model size, not a differentiable parameter.";
        ParameterTarget tg;
        tg.ref = ref(toks[0], kb.keyword);
        tg.nominalToken = *v;
        p.targets.push_back(tg);
        push(std::move(p));
    }

    void materials() {
        static const std::set<std::string> materialOptions = {
            "*ELASTIC", "*PLASTIC", "*DENSITY", "*EXPANSION", "*HYPERELASTIC", "*USER MATERIAL", "*DEPVAR",
            "*CONDUCTIVITY", "*SPECIFIC HEAT", "*DAMPING", "*VISCOELASTIC", "*CREEP", "*HYPERFOAM",
            "*MULLINS EFFECT", "*USER DEFINED FIELD", "*USER OUTPUT VARIABLES", "*REGULARIZE", "*RATE DEPENDENT",
            "*CYCLIC HARDENING", "*DAMAGE INITIATION", "*DAMAGE EVOLUTION", "*DAMAGE STABILIZATION",
            "*UNIAXIAL TEST DATA", "*BIAXIAL TEST DATA", "*PLANAR TEST DATA", "*VOLUMETRIC TEST DATA",
            "*SHEAR TEST DATA", "*POTENTIAL", "*VISCOUS", "*TRS", "*DRUCKER PRAGER", "*DRUCKER PRAGER HARDENING",
            "*MOHR COULOMB", "*MOHR COULOMB HARDENING", "*CAP PLASTICITY", "*CAP HARDENING", "*POROUS ELASTIC",
            "*SWELLING", "*LATENT HEAT", "*ELECTRICAL CONDUCTIVITY", "*JOULE HEAT FRACTION",
            "*INELASTIC HEAT FRACTION", "*ANISOTROPIC HYPERELASTIC", "*CONCRETE DAMAGED PLASTICITY",
            "*CONCRETE TENSION STIFFENING", "*CONCRETE COMPRESSION HARDENING", "*CONCRETE TENSION DAMAGE",
            "*CONCRETE COMPRESSION DAMAGE", "*HYSTERESIS", "*FAIL STRESS", "*FAIL STRAIN", "*ORNL",
            "*SHEAR FAILURE", "*TENSILE FAILURE", "*CRUSHABLE FOAM", "*CRUSHABLE FOAM HARDENING", "*EOS",
            "*GASKET THICKNESS BEHAVIOR", "*PERMEABILITY", "*SORPTION", "*DIFFUSIVITY", "*SOLUBILITY",
            "*USER DEFINED FIELD", "*PLASTIC AXIAL", "*PLASTIC M1", "*PLASTIC M2", "*PLASTIC TORQUE",
            "*ELECTROMAGNETIC RESPONSE", "*MAGNETIC PERMEABILITY", "*DIELECTRIC", "*PIEZOELECTRIC",
            "*HEAT GENERATION", "*NONLINEAR ISOTROPIC HARDENING"};
        std::string mat;
        for (const auto& kb : deck_.blocks()) {
            if (kb.keyword == "*MATERIAL") {
                mat = unquote(kb.param("NAME").value_or(""));
                continue;
            }
            if (mat.empty()) continue;
            if (!materialOptions.count(kb.keyword)) {
                if (kb.keyword != "*INCLUDE") mat.clear();
                continue;
            }
            if (kb.keyword == "*ELASTIC") {
                elastic(kb, mat);
            } else if (kb.keyword == "*PLASTIC") {
                plastic(kb, mat);
            } else if (kb.keyword == "*DENSITY") {
                table(kb, mat, "mat:" + upper(mat) + ":density",
                      {CoefSpec{"density", true, 0.0, 1e300, "Mass density"}}, "*DENSITY");
            } else if (kb.keyword == "*EXPANSION") {
                const std::string type = normalizeName(kb.param("TYPE").value_or("ISO"));
                std::vector<CoefSpec> s;
                if (type == "ORTHO")
                    s = {CoefSpec{"alpha11", false, -1e300, 1e300, "thermal expansion coefficient alpha11"},
                         CoefSpec{"alpha22", false, -1e300, 1e300, "thermal expansion coefficient alpha22"},
                         CoefSpec{"alpha33", false, -1e300, 1e300, "thermal expansion coefficient alpha33"}};
                else if (type == "ANISO")
                    for (const char* n : {"alpha11", "alpha22", "alpha33", "alpha12", "alpha13", "alpha23"})
                        s.push_back(CoefSpec{n, false, -1e300, 1e300, std::string("thermal expansion coefficient ") + n});
                else if (type == "ISO")
                    s = {CoefSpec{"alpha", false, -1e300, 1e300, "Thermal expansion coefficient"}};
                if (!s.empty()) table(kb, mat, "mat:" + upper(mat) + ":expansion", s, "*EXPANSION, TYPE=" + type);
            } else if (kb.keyword == "*HYPERELASTIC") {
                hyperelastic(kb, mat);
            } else if (kb.keyword == "*USER MATERIAL") {
                userMaterial(kb, mat);
            } else if (kb.keyword == "*DEPVAR") {
                depvar(kb, mat);
            }
        }
    }

    // ------------------------------------------------------------------ sections
    /// First element of an element set named in a block (searching part/instance scoped keys).
    const Element* firstElementOf(const std::string& elset, const KeywordBlock& kb) const {
        const std::string u = upper(unquote(elset));
        const auto& sets = deck_.elementSets();
        auto pick = [&](const std::string& key) -> const Element* {
            auto it = sets.find(key);
            if (it == sets.end() || it->second.empty()) return nullptr;
            return &deck_.mesh().elements[it->second.front()];
        };
        if (!kb.instanceName.empty())
            if (auto e = pick(upper(kb.instanceName) + "." + u)) return e;
        if (auto e = pick(u)) return e;
        for (const auto& [key, v] : sets) {
            if (key.size() > u.size() + 1 && key.compare(key.size() - u.size(), u.size(), u) == 0 &&
                key[key.size() - u.size() - 1] == '.' && !v.empty())
                return &deck_.mesh().elements[v.front()];
        }
        return nullptr;
    }

    void sectionValue(const KeywordBlock& kb, const Tok& t, const std::string& elset, const std::string& what,
                      const std::string& idSuffix, bool positive, const std::string& meaning) {
        ParameterInfo p;
        p.id = "sec:" + upper(elset) + ":" + idSuffix;
        p.name = "Section " + elset + ": " + what;
        p.description = meaning + " of the " + kb.keyword.substr(1) + " assigned to element set " + elset +
                        (kb.partName.empty() ? "" : " in part " + kb.partName) + ".";
        p.category = ParamCategory::Section;
        p.confidence = Confidence::KnownSemantic;
        p.mustBePositive = positive;
        if (positive) p.lowerBound = 0.0;
        const auto mat = kb.param("MATERIAL");
        p.association = "elset " + elset + (mat ? ", material " + unquote(*mat) : "");
        if (!kb.partName.empty()) p.association += ", part " + kb.partName;
        p.steps = allSteps_;
        single(std::move(p), t, kb.keyword);
    }

    void sections() {
        for (const auto& kb : deck_.blocks()) {
            const std::string elset = unquote(kb.param("ELSET").value_or(""));
            if (kb.keyword == "*SOLID SECTION" || kb.keyword == "*SHELL SECTION") {
                if (kb.data.empty()) continue;
                if (kb.hasParam("COMPOSITE")) {
                    for (std::size_t r = 0; r < kb.data.size(); ++r) {
                        const auto f = splitFields(kb.data[r].text);
                        if (f.empty()) continue;
                        sectionValue(kb, Tok{kb.data[r].fileIndex, kb.data[r].lineNumber, 0, f[0]}, elset,
                                     "ply " + std::to_string(r + 1) + " thickness",
                                     "ply" + std::to_string(r + 1) + ":thickness", true,
                                     "Thickness of ply " + std::to_string(r + 1));
                    }
                    continue;
                }
                const auto f = splitFields(kb.data[0].text);
                if (f.empty() || !parseNumber(f[0])) continue;
                const Tok t{kb.data[0].fileIndex, kb.data[0].lineNumber, 0, f[0]};
                if (kb.keyword == "*SHELL SECTION") {
                    if (kb.hasParam("NODAL THICKNESS")) continue;
                    sectionValue(kb, t, elset, "thickness", "thickness", true, "Shell thickness");
                    continue;
                }
                const Element* e = firstElementOf(elset, kb);
                const ElementFamily fam = e ? e->info.family : ElementFamily::Unknown;
                if (fam == ElementFamily::Truss)
                    sectionValue(kb, t, elset, "area", "area", true, "Cross-sectional area");
                else if (fam == ElementFamily::Continuum2D)
                    sectionValue(kb, t, elset, "thickness", "thickness", true, "Out-of-plane thickness");
                else if (fam == ElementFamily::Unknown || fam == ElementFamily::User)
                    sectionValue(kb, t, elset, "thickness/area", "thickness", true,
                                 "First section data value (thickness for plane elements, area for trusses)");
                // 3D continua and axisymmetric elements: the value has no effect; not exposed.
            } else if (kb.keyword == "*MEMBRANE SECTION" || kb.keyword == "*SHELL GENERAL SECTION") {
                if (kb.data.empty()) continue;
                const auto f = splitFields(kb.data[0].text);
                if (f.empty() || !parseNumber(f[0])) continue;
                sectionValue(kb, Tok{kb.data[0].fileIndex, kb.data[0].lineNumber, 0, f[0]}, elset, "thickness",
                             "thickness", true, kb.keyword == "*MEMBRANE SECTION" ? "Membrane thickness" : "Shell thickness");
            } else if (kb.keyword == "*BEAM SECTION") {
                if (kb.data.empty()) continue;
                const std::string sec = normalizeName(kb.param("SECTION").value_or(""));
                static const std::map<std::string, std::vector<std::string>> dims = {
                    {"RECT", {"width a", "height b"}},
                    {"CIRC", {"radius r"}},
                    {"PIPE", {"outer radius r", "wall thickness t"}},
                    {"BOX", {"width a", "height b", "wall t1", "wall t2", "wall t3", "wall t4"}},
                    {"HEX", {"outer radius r", "wall thickness t"}},
                    {"I", {"offset l", "height h", "flange width b1", "flange width b2", "flange t1", "flange t2",
                           "web t3"}},
                    {"L", {"width a", "height b", "thickness t1", "thickness t2"}},
                    {"TRAPEZOID", {"width a", "height b", "width c", "offset d"}}};
                const auto f = splitFields(kb.data[0].text);
                auto it = dims.find(sec);
                for (std::size_t k = 0; k < f.size(); ++k) {
                    if (!parseNumber(f[k])) continue;
                    const std::string nm = (it != dims.end() && k < it->second.size()) ? it->second[k]
                                                                                       : "dimension " + std::to_string(k + 1);
                    const bool positive = !(sec == "I" && k == 0) && !(sec == "TRAPEZOID" && k == 3);
                    sectionValue(kb, Tok{kb.data[0].fileIndex, kb.data[0].lineNumber, static_cast<int>(k), f[k]},
                                 elset, sec + " " + nm, "beam:" + std::to_string(k + 1), positive,
                                 "Beam cross-section dimension '" + nm + "'");
                }
            }
        }
    }

    // ------------------------------------------------------------------ loads
    void loads() {
        for (const auto& kb : deck_.blocks()) {
            const bool cload = kb.keyword == "*CLOAD";
            const bool dload = kb.keyword == "*DLOAD";
            const bool dsload = kb.keyword == "*DSLOAD";
            if (!cload && !dload && !dsload) continue;
            const std::string amp = unquote(kb.param("AMPLITUDE").value_or(""));
            const std::string step = stepName(kb.stepIndex);
            for (const auto& ln : kb.data) {
                const auto f = splitFields(ln.text);
                if (f.size() < 3) continue;
                const std::string target = f[0];
                ParameterInfo p;
                p.category = ParamCategory::Load;
                p.confidence = Confidence::KnownSemantic;
                p.steps = stepsFrom(kb.stepIndex, kb.keyword);
                p.association = (cload ? "node/nset " : dsload ? "surface " : "element/elset/surface ") + target +
                                ", step " + step;
                std::string kind;
                if (cload) {
                    const auto dof = parseInteger(f[1]);
                    if (!dof) continue;
                    kind = (*dof <= 3 ? "CF" + std::to_string(*dof) : "CM" + std::to_string(*dof - 3));
                    p.id = "load:" + upper(step) + ":cload:" + upper(target) + ":" + std::to_string(*dof);
                    p.name = kind + " at " + target + " (" + step + ")";
                    p.description = "Concentrated " + std::string(*dof <= 3 ? "force" : "moment") + " component " +
                                    std::to_string(*dof) + " applied to " + target + " in step " + step;
                } else {
                    const std::string label = upper(f[1]);
                    std::string what = "Distributed load";
                    if (label.rfind("P", 0) == 0 || label == "HP") what = "Pressure";
                    if (label == "GRAV") what = "Gravity";
                    if (label == "BX" || label == "BY" || label == "BZ" || label == "BR") what = "Body force";
                    if (label == "CENTRIF" || label == "ROTA") what = "Rotational body load";
                    if (label == "TRVEC" || label == "TRSHR" || label == "TRNOR") what = "Surface traction";
                    p.id = "load:" + upper(step) + (dload ? ":dload:" : ":dsload:") + upper(target) + ":" + label;
                    p.name = what + " " + label + " on " + target + " (" + step + ")";
                    p.description = what + " magnitude (load type " + label + ") on " + target + " in step " + step;
                }
                if (!amp.empty()) p.description += ", scaled in time by amplitude " + amp;
                p.description += ". Loads persist into later steps unless redefined";
                single(std::move(p), Tok{ln.fileIndex, ln.lineNumber, 2, f[2]}, kb.keyword);
            }
        }
    }

    void boundaryConditions() {
        for (const auto& be : deck_.boundaries()) {
            if (be.valueRef.field < 0 || be.magnitude == 0.0) continue;
            const KeywordBlock& kb = deck_.blocks()[be.blockIndex];
            const std::string step = stepName(be.stepIndex);
            std::string dofs = dofLabel(be.firstDof, be.type);
            if (be.lastDof != be.firstDof) dofs += "-" + dofLabel(be.lastDof, be.type);
            ParameterInfo p;
            p.id = "bc:" + upper(step) + ":" + upper(be.target) + ":" + std::to_string(be.firstDof) + "-" +
                   std::to_string(be.lastDof);
            p.name = "Prescribed " + dofs + " at " + be.target + " (" + step + ")";
            std::string kind = be.type == "DISPLACEMENT" ? "displacement" : upper(be.type) == "VELOCITY" ? "velocity"
                               : be.type == "ACCELERATION" ? "acceleration" : be.type;
            p.description = "Prescribed " + kind + " value for degree(s) of freedom " + std::to_string(be.firstDof) +
                            (be.lastDof != be.firstDof ? "-" + std::to_string(be.lastDof) : "") + " at " + be.target +
                            (be.stepIndex < 0 ? " (model data)" : " in step " + step) +
                            (be.amplitude.empty() ? "" : ", scaled in time by amplitude " + be.amplitude) +
                            " (" + std::to_string(be.nodes.size()) + " node(s))";
            p.category = ParamCategory::Boundary;
            p.confidence = Confidence::KnownSemantic;
            p.association = "node/nset " + be.target + ", step " + step;
            p.steps = stepsFrom(be.stepIndex, kb.keyword);
            if (be.nodes.empty()) p.note = "Target '" + be.target + "' could not be resolved to mesh nodes.";
            const SourceRef& r = be.valueRef;
            int fi = 0;
            for (int i = 0; i < static_cast<int>(deck_.files().size()); ++i)
                if (deck_.files()[i].path == r.file) fi = i;
            const auto f = splitFields(deck_.files()[fi].lines[r.line - 1]);
            single(std::move(p), Tok{fi, r.line, r.field, f.size() > 3 ? f[3] : std::string()}, kb.keyword);
        }
    }

    // ------------------------------------------------------------------ amplitudes
    void amplitudes() {
        // Steps that reference each amplitude.
        std::map<std::string, std::set<int>> usedIn;
        for (const auto& kb : deck_.blocks())
            if (auto a = kb.param("AMPLITUDE"); a && kb.stepIndex >= 0) usedIn[upper(unquote(*a))].insert(kb.stepIndex);
        for (const auto& kb : deck_.blocks()) {
            if (kb.keyword != "*AMPLITUDE") continue;
            const std::string name = unquote(kb.param("NAME").value_or(""));
            const std::string def = normalizeName(kb.param("DEFINITION").value_or("TABULAR"));
            if (def != "TABULAR" && def != "SMOOTH STEP") continue;
            if (kb.hasParam("INPUT")) {
                // data came from a file: still token-addressable, handled below like inline data
            }
            const auto toks = flatTokens(kb);
            for (std::size_t k = 0; k + 1 < toks.size(); k += 2) {
                const auto t = parseNumber(toks[k].text);
                ParameterInfo p;
                p.id = "amp:" + upper(name) + ":" + std::to_string(k / 2 + 1);
                p.name = "Amplitude " + name + ": a(t=" + (t ? displayNumber(*t) : toks[k].text) + ")";
                p.description = "Value of amplitude curve " + name + " (" + def + ") at time point " +
                                std::to_string(k / 2 + 1) + "; it multiplies the loads/boundary conditions that use it";
                p.category = ParamCategory::Amplitude;
                p.confidence = Confidence::KnownSemantic;
                p.association = "amplitude " + name;
                for (int s : usedIn[upper(name)])
                    if (s < static_cast<int>(allSteps_.size())) p.steps.push_back(allSteps_[s]);
                if (p.steps.empty()) p.note = "Amplitude is not referenced by any step-level load or boundary condition.";
                single(std::move(p), toks[k + 1], kb.keyword);
            }
        }
    }

    // ------------------------------------------------------------------ user elements
    void userElements() {
        struct UeDef {
            int nReal = -1, nInt = 0;
        };
        std::map<std::string, UeDef> defs;
        for (const auto& kb : deck_.blocks())
            if (kb.keyword == "*USER ELEMENT")
                defs[upper(unquote(kb.param("TYPE").value_or("")))] = {intParam(kb, "PROPERTIES", 0),
                                                                       intParam(kb, "I PROPERTIES", 0)};
        for (const auto& kb : deck_.blocks()) {
            if (kb.keyword != "*UEL PROPERTY") continue;
            const std::string elset = unquote(kb.param("ELSET").value_or(""));
            UeDef def;
            std::string type;
            if (const Element* e = firstElementOf(elset, kb)) {
                type = e->type;
                auto it = defs.find(type);
                if (it != defs.end()) def = it->second;
            } else if (defs.size() == 1) {
                type = defs.begin()->first;
                def = defs.begin()->second;
            }
            const auto toks = flatTokens(kb);
            std::string note;
            if (def.nReal < 0) {
                def.nReal = static_cast<int>(toks.size());
                note = "Could not match ELSET " + elset + " to a *USER ELEMENT definition; all values treated as real "
                       "PROPS.";
            } else if (static_cast<int>(toks.size()) != def.nReal + def.nInt) {
                note = "*USER ELEMENT declares PROPERTIES=" + std::to_string(def.nReal) + " and I PROPERTIES=" +
                       std::to_string(def.nInt) + " but " + std::to_string(toks.size()) + " values are given.";
            }
            for (std::size_t k = 0; k < toks.size(); ++k) {
                const bool real = static_cast<int>(k) < def.nReal;
                const int idx = real ? static_cast<int>(k) + 1 : static_cast<int>(k) - def.nReal + 1;
                if (!real && idx > def.nInt && def.nInt > 0) break;
                ParameterInfo p;
                p.id = "uel:" + upper(elset) + (real ? ":props:" : ":jprops:") + std::to_string(idx);
                p.name = "UEL " + elset + ": " + (real ? "PROPS[" : "JPROPS[") + std::to_string(idx) + "]";
                p.description = std::string(real ? "Real" : "Integer") + " property number " + std::to_string(idx) +
                                " (1-based " + (real ? "PROPS" : "JPROPS") + "(" + std::to_string(idx) +
                                ")) passed to user element subroutine UEL" + (type.empty() ? "" : " for type " + type) +
                                " on element set " + elset + ". Its meaning is defined only by the compiled UEL.";
                p.category = ParamCategory::UserElement;
                p.confidence = Confidence::OpaqueIndexed;
                p.association = "elset " + elset + (type.empty() ? "" : ", type " + type);
                p.steps = allSteps_;
                p.note = note;
                if (!real) {
                    p.supported = false;
                    p.note = "Integer property (JPROPS): cannot be differentiated." + (note.empty() ? "" : " " + note);
                }
                single(std::move(p), toks[k], kb.keyword);
            }
        }
    }

    // ------------------------------------------------------------------ *PARAMETER
    void parameters() {
        for (const auto& kb : deck_.blocks()) {
            if (kb.keyword != "*PARAMETER") continue;
            for (const auto& ln : kb.data) {
                const std::string t = trim(ln.text);
                const std::size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                const std::string name = trim(t.substr(0, eq));
                const std::string rhs = trim(t.substr(eq + 1));
                const auto v = parseNumber(rhs);
                if (!v || name.empty()) continue;
                ParameterInfo p;
                p.id = "param:" + name;
                p.name = "*PARAMETER " + name;
                p.description = "Scalar defined by *PARAMETER (" + name + " = " + rhs +
                                "); it changes every place where <" + name + "> is used in the deck";
                p.category = ParamCategory::Custom;
                p.confidence = Confidence::KnownSemantic;
                p.association = "parameter " + name;
                p.steps = allSteps_;
                p.note = "Parametrized decks are partially supported: Abaqus evaluates the substitutions; ABSS does not.";
                single(std::move(p), Tok{ln.fileIndex, ln.lineNumber, 0, rhs}, kb.keyword);
            }
        }
    }

    // ------------------------------------------------------------------ geometry
    void geometry() {
        const Mesh& m = deck_.mesh();
        if (m.nodeCount() == 0) return;
        for (const auto& [setName, nodes] : deck_.nodeSets()) {
            if (nodes.empty()) continue;
            for (int d = 1; d <= m.spatialDim; ++d) {
                ParameterInfo p;
                try {
                    p = makeGeometryParameter(deck_, nodes, d, "Move set " + setName + " in " + axisName(d));
                } catch (const std::exception&) {
                    continue;
                }
                p.id = "geom:" + setName + ":" + axisName(d);
                p.association = "nset " + setName;
                push(std::move(p));
            }
        }
    }
};

}  // namespace

std::vector<ParameterInfo> discoverParameters(const Deck& deck) { return Discoverer(deck).run(); }

ParameterInfo makeGeometryParameter(const Deck& deck, const std::vector<std::int32_t>& nodes, int direction,
                                    const std::string& name) {
    if (nodes.empty()) throw std::invalid_argument("makeGeometryParameter: empty node list");
    if (direction < 1 || direction > 3) throw std::invalid_argument("makeGeometryParameter: direction must be 1..3");
    const Mesh& m = deck.mesh();
    std::set<std::tuple<int, int, int>> seen;
    ParameterInfo p;
    p.name = name;
    p.category = ParamCategory::Geometry;
    p.confidence = Confidence::KnownSemantic;
    p.nominal = 0.0;
    p.mode = EditMode::Offset;
    const double L = m.characteristicLength() > 0 ? m.characteristicLength() : 1.0;
    p.lowerBound = -0.1 * L;
    p.upperBound = 0.1 * L;
    for (const auto& s : deck.steps()) p.steps.push_back(s.name);
    bool partLevel = false;
    std::set<std::int32_t> rotatedInstances;
    for (const auto& kb : deck.blocks())
        if (kb.keyword == "*INSTANCE" && kb.data.size() > 1)
            for (std::size_t i = 0; i < m.instanceNames.size(); ++i)
                if (iequals(m.instanceNames[i], unquote(kb.param("NAME").value_or(""))))
                    rotatedInstances.insert(static_cast<std::int32_t>(i));
    bool rotated = false;
    for (std::int32_t n : nodes) {
        if (n < 0 || n >= m.nodeCount()) throw std::invalid_argument("makeGeometryParameter: bad node index");
        const LineLoc loc = deck.nodeDefinition(n);
        if (loc.fileIndex < 0) continue;
        if (m.nodeKeys[n].instance >= 0) partLevel = true;
        if (rotatedInstances.count(m.nodeKeys[n].instance)) rotated = true;
        if (!seen.insert({loc.fileIndex, loc.line, direction}).second) continue;
        const std::string& text = deck.files()[loc.fileIndex].lines[loc.line - 1];
        const auto f = splitFields(text);
        double nominal = 0.0;
        if (direction < static_cast<int>(f.size())) {
            const auto v = parseNumber(f[direction]);
            if (!v && !f[direction].empty()) continue;  // parametrized coordinate: cannot edit
            nominal = v.value_or(0.0);
        } else if (direction > static_cast<int>(f.size())) {
            continue;  // would need padding fields; skip
        }
        ParameterTarget t;
        t.ref.file = deck.files()[loc.fileIndex].path;
        t.ref.line = loc.line;
        t.ref.field = direction;
        t.ref.keyword = "*NODE";
        t.nominalToken = nominal;
        t.weight = 1.0;
        p.targets.push_back(t);
    }
    if (p.targets.empty()) throw std::invalid_argument("makeGeometryParameter: no editable node coordinates");
    p.description = "Rigidly moves " + std::to_string(p.targets.size()) + " node definition(s) by the parameter value in the " +
                    axisName(direction) + " direction (offset from the original coordinates; 0 = unchanged). "
                    "Valid only while the mesh topology stays the same.";
    p.note = "Geometry perturbation: requires the same mesh topology; large values can distort elements.";
    if (partLevel)
        p.note += " Coordinates are edited in the *PART definition, so every instance of that part moves.";
    if (rotated) p.note += " Some nodes belong to rotated instances: the direction is in part-local coordinates.";
    p.id = "geom:custom:" + name;
    p.association = std::to_string(nodes.size()) + " node(s)";
    return p;
}

ParameterInfo makeCustomParameter(const Deck& deck, const SourceRef& r, const std::string& name) {
    int fi = -1;
    const std::string want = absoluteNormal(r.file);
    for (int i = 0; i < static_cast<int>(deck.files().size()); ++i)
        if (samePath(deck.files()[i].path, want)) {
            fi = i;
            break;
        }
    if (fi < 0) throw std::invalid_argument("makeCustomParameter: file is not part of the deck: " + r.file);
    if (r.line < 1 || r.line > static_cast<int>(deck.files()[fi].lines.size()))
        throw std::invalid_argument("makeCustomParameter: line out of range");
    const std::string& text = deck.files()[fi].lines[r.line - 1];
    const std::string t = trim(text);
    if (t.empty() || t[0] == '*') throw std::invalid_argument("makeCustomParameter: not a data line");
    const auto f = splitFields(text);
    if (r.field < 0 || r.field >= static_cast<int>(f.size()))
        throw std::invalid_argument("makeCustomParameter: field out of range");
    const auto v = parseNumber(f[r.field]);
    if (!v) throw std::invalid_argument("makeCustomParameter: field is not numeric: '" + f[r.field] + "'");
    // Owning keyword: closest keyword line above in the same file.
    std::string kw = r.keyword;
    if (kw.empty())
        for (const auto& kb : deck.blocks())
            for (const auto& ln : kb.data)
                if (ln.fileIndex == fi && ln.lineNumber == r.line) kw = kb.keyword;
    ParameterInfo p;
    p.id = "custom:" + fromPath(toPath(deck.files()[fi].path).filename()) + ":" + std::to_string(r.line) + ":" +
           std::to_string(r.field);
    p.name = name.empty() ? "Custom " + p.id.substr(7) : name;
    p.description = "User-defined scalar controlling field " + std::to_string(r.field + 1) + " of line " +
                    std::to_string(r.line) + " in " + fromPath(toPath(deck.files()[fi].path).filename()) +
                    (kw.empty() ? "" : " (" + kw + ")") + ". ABSS does not know its physical meaning.";
    p.category = ParamCategory::Custom;
    p.confidence = Confidence::OpaqueIndexed;
    p.nominal = *v;
    p.association = kw;
    for (const auto& s : deck.steps()) p.steps.push_back(s.name);
    ParameterTarget tg;
    tg.ref.file = deck.files()[fi].path;
    tg.ref.line = r.line;
    tg.ref.field = r.field;
    tg.ref.keyword = kw;
    tg.nominalToken = *v;
    p.targets.push_back(tg);
    return p;
}

}  // namespace abss::inp
