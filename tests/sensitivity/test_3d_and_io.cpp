#include "abss/core/project_io.hpp"
#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace abss;

TEST_CASE("3D cantilever (hexahedra, J2, unload/reload): orders 1-2 match full reruns incl. geometry parameter") {
    synthetic::SyntheticOracle o(synthetic::cantileverBlock(6, 2, 2));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0, 2, 4, 5};  // E, sigma_y, F_tip, L_beam
    req.maxOrder = 2;
    RunControl ctl;
    const auto rep = runSensitivities(o, u, req, s, ctl);
    CHECK(rep.frames.size() == 8);
    ValidationOptions opt;  // shipping tolerances (1e-3, 1e-2); L_beam reaches 9.6e-4 / 1.2e-3 at default steps
    MESSAGE(validateAgainstReruns(o, req, s, opt, ctl));
    for (int p : req.parameters) {
        CHECK(s.quality[DerivativeSpec::pure(p, 1)].status == QualityStatus::Validated);
        CHECK(s.quality[DerivativeSpec::pure(p, 2)].status == QualityStatus::Validated);
    }
}

TEST_CASE("Mises sensitivity is the derivative of the invariant, not the invariant of the stress derivative") {
    synthetic::SyntheticOracle o(synthetic::singleQuad(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {2};  // g (prescribed top displacement)
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    const int fm = s.fieldIndex("S_Mises"), fs = s.fieldIndex("S");
    const auto& dm = *s.get(fm, DerivativeSpec::pure(2, 1), 2);
    const auto& m0 = *s.get(fm, DerivativeSpec::nominal(), 2);
    const auto& dS = *s.get(fs, DerivativeSpec::pure(2, 1), 2);
    // Linear elastic response proportional to g: dMises/dg = Mises/g exactly.
    CHECK(dm[0] == doctest::Approx(m0[0] / 0.002).epsilon(1e-6));
    // ... plane strain with a free lateral edge (sigma11 = 0, eps33 = 0): dS22/dg = E/(1-nu^2) at amplitude 1
    CHECK(dS[1] == doctest::Approx(200000.0 / (1 - 0.3 * 0.3)).epsilon(1e-6));
}

TEST_CASE("Project save / reopen preserves mesh, frames, all series and evidence") {
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(true));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0, 4};
    req.maxOrder = 2;
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    ValidationOptions opt;
    validateAgainstReruns(o, req, s, opt, ctl);
    const auto path = (std::filesystem::temp_directory_path() / "abss_roundtrip test.abss").string();
    ProjectMeta meta;
    meta.solverBackend = "synthetic";
    saveProject(path, s, meta);
    ProjectMeta m2;
    const auto r = loadProject(path, &m2);
    CHECK(m2.solverBackend == "synthetic");
    CHECK(r.mesh.nodeCount() == s.mesh.nodeCount());
    CHECK(r.frames.size() == s.frames.size());
    CHECK(r.parameters.size() == s.parameters.size());
    CHECK(r.synthetic);
    for (const auto& d : s.derivatives())
        for (int f = 0; f < static_cast<int>(s.fields.size()); ++f)
            for (int fr = 0; fr < static_cast<int>(s.frames.size()); ++fr) {
                const auto* a = s.get(f, d, fr);
                const auto* b = r.get(f, d, fr);
                REQUIRE((a == nullptr) == (b == nullptr));
                if (a) CHECK(*a == *b);  // bitwise identical doubles
            }
    CHECK(r.quality.at(DerivativeSpec::pure(0, 1)).status == s.quality.at(DerivativeSpec::pure(0, 1)).status);
    const auto csv = historyCsv(r, r.fieldIndex("U"), 1, 0, {DerivativeSpec::nominal(), DerivativeSpec::pure(0, 1)});
    CHECK(csv.find("dU1/dE") != std::string::npos);
    std::remove(path.c_str());
}


TEST_CASE("Default cantilever demo (10x3x3 hexes, default steps) validates; reruns converge at the round-off floor") {
    synthetic::SyntheticOracle o(synthetic::cantileverBlock());
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0, 4};  // E, F_tip with the default step policy
    RunControl ctl;
    const auto rep = runSensitivities(o, u, req, s, ctl);
    CHECK(rep.oracleEvaluations > 0);
    ValidationOptions opt;
    MESSAGE(validateAgainstReruns(o, req, s, opt, ctl));
    for (int p : req.parameters) CHECK(s.quality.at(DerivativeSpec::pure(p, 1)).status == QualityStatus::Validated);
}