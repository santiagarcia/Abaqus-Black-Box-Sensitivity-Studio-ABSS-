// Requires a licensed Abaqus (ctest label "abaqus"; run with: ctest -L abaqus). Never runs in default CI.
#include "abss/inp/deck.hpp"
#include "abss/oracle/abaqus.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace abss;

TEST_CASE("Abaqus T2D2 truss: analytic du/dE, d2u/dE2, du/dF, tangent check through the real oracle") {
    auto inst = detectAbaqus();
    if (!inst.found) {
        MESSAGE("Abaqus not found: test skipped");
        return;
    }
    const std::string inp = std::string(ABSS_EXAMPLES_DIR) + "/models/truss.inp";
    const auto deck = inp::Deck::load(inp);
    AbaqusRunConfig cfg;
    cfg.inpPath = inp;
    cfg.abaqusCommand = inst.command;
    cfg.scratchDir = (std::filesystem::temp_directory_path() / "abss_ctest_truss").string();
    std::filesystem::remove_all(cfg.scratchDir);
    cfg.parameters = inp::discoverParameters(deck);
    cfg.maxConcurrentJobs = 4;
    auto oracle = makeAbaqusOracle(cfg);
    ResultStore store;
    auto u = oracle->runNominal(store, nullptr);
    int pE = -1, pF = -1;
    for (size_t i = 0; i < store.parameters.size(); ++i) {
        if (store.parameters[i].id == "mat:STEEL:elastic:E") pE = static_cast<int>(i);
        if (store.parameters[i].id == "load:LOAD:cload:TIP:1") pF = static_cast<int>(i);
    }
    REQUIRE(pE >= 0);
    REQUIRE(pF >= 0);
    SensitivityRequest req;
    req.parameters = {pE, pF};
    req.maxOrder = 2;
    req.parallelBranches = true;
    RunControl ctl;
    const auto rep = runSensitivities(*oracle, u, req, store, ctl);
    CHECK(rep.frames.at(0).tangentCheckError < 1e-8);
    const int fU = store.fieldIndex("U");
    const auto* d1 = store.get(fU, DerivativeSpec::pure(pE, 1), 1);
    const auto* d2 = store.get(fU, DerivativeSpec::pure(pE, 2), 1);
    const auto* dF = store.get(fU, DerivativeSpec::pure(pF, 1), 1);
    REQUIRE(d1);
    // node 2 (index 1), U1: u = F L/(E A) with F = 1000, L = 100, E = 2e5, A = 10
    CHECK((*d1)[2] == doctest::Approx(-2.5e-7).epsilon(1e-6));
    CHECK((*d2)[2] == doctest::Approx(2.5e-12).epsilon(1e-3));
    CHECK((*dF)[2] == doctest::Approx(5e-5).epsilon(1e-9));
}
