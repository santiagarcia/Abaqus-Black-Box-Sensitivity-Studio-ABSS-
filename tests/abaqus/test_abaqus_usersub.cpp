// Real Abaqus + user subroutines (ctest label "abaqus"). For a UMAT and a UEL, three delivery forms are tested:
//   source (.f)   -> ABSS builds standardU.dll once (`abaqus make` if the Abaqus user-subroutine files are installed,
//                    otherwise its direct build with ifort/ifx + MSVC link)
//   object (.obj) -> only the object file is given (no source next to it), ABSS links it
//   library       -> only a folder with the built DLLs (no source, no object, no compiler needed at run time)
// Sensitivities w.r.t. opaque PROPS are checked against analytic answers and must agree between the three forms.
#include "abss/inp/deck.hpp"
#include "abss/oracle/abaqus.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>

namespace fs = std::filesystem;
using namespace abss;

namespace {

struct Run {
    ResultStore store;
    SensitivityReport rep;
};

Run runModel(const std::string& inp, const std::string& usub, const std::vector<std::string>& ids, const std::string& tag) {
    const auto inst = detectAbaqus();
    const auto deck = inp::Deck::load(inp);
    AbaqusRunConfig cfg;
    cfg.inpPath = inp;
    cfg.abaqusCommand = inst.command;
    cfg.scratchDir = (fs::temp_directory_path() / ("abss_ctest_usub_" + tag)).string();
    fs::remove_all(cfg.scratchDir);
    cfg.parameters = inp::discoverParameters(deck);
    cfg.userSubroutine = usub;
    cfg.maxConcurrentJobs = 4;
    auto oracle = makeAbaqusOracle(cfg);
    Run r;
    auto u = oracle->runNominal(r.store, nullptr);
    SensitivityRequest req;
    for (const auto& id : ids)
        for (size_t i = 0; i < r.store.parameters.size(); ++i)
            if (r.store.parameters[i].id == id) req.parameters.push_back(static_cast<int>(i));
    REQUIRE(req.parameters.size() == ids.size());
    req.parallelBranches = true;
    RunControl ctl;
    r.rep = runSensitivities(*oracle, u, req, r.store, ctl);
    return r;
}

int idx(const ResultStore& s, const std::string& id) {
    for (size_t i = 0; i < s.parameters.size(); ++i)
        if (s.parameters[i].id == id) return static_cast<int>(i);
    FAIL("parameter not found: " << id);
    return -1;
}

double nodal(const ResultStore& s, const std::string& field, int param, int frame, std::int64_t label, int comp) {
    const int f = s.fieldIndex(field);
    const auto* v = s.get(f, DerivativeSpec::pure(param, 1), frame);
    REQUIRE(v);
    const auto n = s.mesh.findNode({-1, label});
    REQUIRE(n >= 0);
    return (*v)[static_cast<size_t>(n * s.fields[static_cast<size_t>(f)].componentCount() + comp)];
}

// Build standardU.dll + .obj from the example source, then stage the three delivery forms in separate folders.
struct Forms {
    std::string source, object, libraryDir;
};
Forms stage(const std::string& srcName, const std::string& tag) {
    const auto inst = detectAbaqus();
    const fs::path base = fs::temp_directory_path() / ("abss_usub_forms_" + tag);
    fs::remove_all(base);
    fs::create_directories(base / "build");
    const fs::path src = fs::path(ABSS_EXAMPLES_DIR) / "subroutines" / srcName;
    const auto lib = prepareUserSubroutine(inst.command, src.string(), (base / "build").string(), nullptr);
    Forms f;
    f.source = src.string();
    // object-only folder (no source next to it)
    fs::create_directories(base / "object_only");
    for (const auto& e : fs::directory_iterator(base / "build"))
        if (e.path().extension() == ".obj") {
            fs::copy_file(e.path(), base / "object_only" / e.path().filename());
            f.object = (base / "object_only" / e.path().filename()).string();
        }
    // library-only folder: the DLLs only (standardU.dll, plus abss_abqstub.dll for a direct build) - no source, no .obj
    fs::create_directories(base / "library_only");
    for (const auto& e : fs::directory_iterator(lib.libraryDir))
        if (e.path().extension() == ".dll") fs::copy_file(e.path(), base / "library_only" / e.path().filename());
    f.libraryDir = (base / "library_only").string();
    return f;
}

bool toolchainReady() {
    if (!detectAbaqus().found) {
        MESSAGE("Abaqus not found: skipped");
        return false;
    }
    const auto tc = detectCompilerToolchain();
    if (!tc.msvc || !tc.intelFortran) {
        MESSAGE("No Fortran/linker toolchain (" << tc.message << "): skipped");
        return false;
    }
    return true;
}

}  // namespace

TEST_CASE("UMAT (opaque PROPS): source, object-only and library-only give the analytic sensitivities") {
    if (!toolchainReady()) return;
    const auto forms = stage("umat_elastic.f", "umat");
    REQUIRE_FALSE(forms.object.empty());
    for (const auto& file : {forms.object, forms.libraryDir})
        for (const auto& p : fs::directory_iterator(fs::path(file).has_extension() ? fs::path(file).parent_path() : fs::path(file)))
            CHECK(p.path().extension() != ".f");  // no source code in the precompiled folders
    const std::string inp = std::string(ABSS_EXAMPLES_DIR) + "/models/umat_props.inp";
    const std::vector<std::string> ids = {"umat:UMAT_MAT:props:1", "umat:UMAT_MAT:props:2"};
    std::map<std::string, Run> runs;
    runs["source"] = runModel(inp, forms.source, ids, "umat_src");
    runs["object"] = runModel(inp, forms.object, ids, "umat_obj");
    runs["library"] = runModel(inp, forms.libraryDir, ids, "umat_lib");
    // Cube 1x1x1, uniaxial stress: eps = 0.001 * t (4 increments). Node 3 = (1,1,0): U2 = -nu * eps.
    for (const auto& [name, r] : runs) {
        CAPTURE(name);
        const int last = static_cast<int>(r.store.frames.size()) - 1;
        CHECK(last == 4);
        const int pE = idx(r.store, ids[0]), pNu = idx(r.store, ids[1]);
        CHECK(nodal(r.store, "U", pNu, last, 3, 1) == doctest::Approx(-0.001).epsilon(1e-6));  // dU2/dPROPS2
        CHECK(std::abs(nodal(r.store, "U", pE, last, 3, 1)) < 1e-12);                          // U2 independent of E
        // Reaction at the pulled face: RF1 per node = E eps / 4 -> d/dE = eps/4
        CHECK(nodal(r.store, "RF", pE, last, 3, 0) == doctest::Approx(0.001 / 4).epsilon(1e-5));
        CHECK(r.rep.frames.at(0).tangentCheckError < 1e-6);
    }
    // The three delivery forms must agree.
    const int fU = runs["source"].store.fieldIndex("U");
    const int pNu = idx(runs["source"].store, ids[1]);
    const auto* a = runs["source"].store.get(fU, DerivativeSpec::pure(pNu, 1), 4);
    for (const char* n : {"object", "library"}) {
        const auto* b = runs[n].store.get(fU, DerivativeSpec::pure(pNu, 1), 4);
        for (size_t i = 0; i < a->size(); ++i) CHECK((*b)[i] == doctest::Approx((*a)[i]).epsilon(1e-10).scale(1e-12));
    }
}

TEST_CASE("UEL (opaque PROPS): source, object-only and library-only give the analytic truss sensitivities") {
    if (!toolchainReady()) return;
    const auto forms = stage("uel_truss.f", "uel");
    REQUIRE_FALSE(forms.object.empty());
    const std::string inp = std::string(ABSS_EXAMPLES_DIR) + "/models/uel_props.inp";
    const std::vector<std::string> ids = {"uel:UTRUSS:props:1", "uel:UTRUSS:props:2"};
    for (const auto& [name, usub] : std::map<std::string, std::string>{
             {"source", forms.source}, {"object", forms.object}, {"library", forms.libraryDir}}) {
        CAPTURE(name);
        const auto r = runModel(inp, usub, ids, "uel_" + name);
        const int last = static_cast<int>(r.store.frames.size()) - 1;
        // u = F L / (E A) = 1000 * 100 / (2e5 * 10): du/dE = -F L/(E^2 A), du/dA = -F L/(E A^2)
        CHECK(nodal(r.store, "U", idx(r.store, ids[0]), last, 2, 0) == doctest::Approx(-2.5e-7).epsilon(1e-6));
        CHECK(nodal(r.store, "U", idx(r.store, ids[1]), last, 2, 0) == doctest::Approx(-5e-3).epsilon(1e-6));
    }
}
