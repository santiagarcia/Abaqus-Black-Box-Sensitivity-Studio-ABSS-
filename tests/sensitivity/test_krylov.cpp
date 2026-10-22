#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

using namespace abss;

// Abaqus returns the elastic-modulus stiffness under built-in plasticity (scout_abaqus2024.md). The engine must detect
// the inconsistent tangent and still produce validated sensitivities through matrix-free GMRES.
TEST_CASE("Inconsistent (elastic) oracle tangent under plasticity: detected and corrected by matrix-free GMRES") {
    for (bool plate : {false, true}) {
        auto def = plate ? synthetic::plateWithHole(6, 6) : synthetic::twoBarTruss(true);
        def.reportElasticTangent = true;
        synthetic::SyntheticOracle o(def);
        ResultStore s;
        auto u = o.runNominal(s, nullptr);
        SensitivityRequest req;
        req.parameters = plate ? std::vector<int>{0, 2} : std::vector<int>{0, 4, 5};
        req.maxOrder = 2;
        if (!plate)  // sigma_y'' ~ 0 on the truss: use h = 1e-3 so the rerun reference noise (tol/h^2) stays below the floor
            for (int p : req.parameters) req.absoluteSteps.push_back(1e-3 * s.parameters[static_cast<size_t>(p)].nominal);
        RunControl ctl;
        const auto rep = runSensitivities(o, u, req, s, ctl);
        // Plastic increments must be detected and corrected: by GMRES or by the true tangent assembled from grouped
        // probes. Here the cost model picks assembly (2 x colours probes << GMRES's ~2 x iterations x right-hand sides),
        // and every assembled tangent must reproduce the independent tangent-check probes.
        int used = 0, probed = 0;
        for (const auto& f : rep.frames) {
            used += f.krylovUsed ? 1 : 0, probed += f.probedTangent ? 1 : 0;
            if (f.probedTangent) CHECK(f.assembledTangentCheck < 1e-4);
            if (plate && f.probedTangent) CHECK(f.tangentColours < 40);  // vs ~80 free DOFs: grouping is real
        }
        CAPTURE(plate);
        CHECK(used + probed > 0);
        CHECK(probed > 0);
        ValidationOptions opt;
        opt.tolerance = {1e-4, 1e-3};
        MESSAGE((plate ? "plate" : "truss") << " with elastic oracle tangent, corrected at " << used + probed << " increments ("
                                            << used << " GMRES, " << probed << " probed tangent)\n"
                                            << validateAgainstReruns(o, req, s, opt, ctl));
        for (int p : req.parameters) {
            CHECK(s.quality.at(DerivativeSpec::pure(p, 1)).status == QualityStatus::Validated);
            CHECK(s.quality.at(DerivativeSpec::pure(p, 2)).status == QualityStatus::Validated);
        }
    }
}

// Performance scheduling (concurrent probe/commit waves, GMRES right-hand sides solved side by side) must not change a
// single number: the same evaluations at the same states, only their order differs.
TEST_CASE("Concurrent waves and concurrent GMRES right-hand sides reproduce the serial engine exactly") {
    auto def = synthetic::plateWithHole(6, 6);
    def.reportElasticTangent = true;
    std::vector<ResultStore> stores(2);
    std::vector<int> matvecs(2, 0);
    for (int run = 0; run < 2; ++run) {
        synthetic::SyntheticOracle o(def);
        auto& s = stores[static_cast<size_t>(run)];
        auto u = o.runNominal(s, nullptr);
        SensitivityRequest req;
        req.parameters = {0, 1, 2};
        req.forceKrylov = true;  // exercise concurrent GMRES right-hand sides
        req.parallelBranches = run == 1;
        RunControl ctl;
        const auto rep = runSensitivities(o, u, req, s, ctl);
        for (const auto& f : rep.frames) matvecs[static_cast<size_t>(run)] += f.krylovMatvecs;
    }
    CHECK(matvecs[0] > 0);
    CHECK(matvecs[0] == matvecs[1]);
    double worst = 0;
    stores[0].forEachSeries([&](int field, const DerivativeSpec& d, int frame, const std::vector<double>& v) {
        const auto* w = stores[1].get(field, d, frame);
        REQUIRE(w);
        REQUIRE(w->size() == v.size());
        for (size_t c = 0; c < v.size(); ++c) worst = std::max(worst, std::abs(v[c] - (*w)[c]));
    });
    CHECK(worst == 0.0);
}

TEST_CASE("Without Krylov the elastic tangent gives wrong sensitivities (the failure is real, not hypothetical)") {
    auto def = synthetic::twoBarTruss(true);
    def.reportElasticTangent = true;
    synthetic::SyntheticOracle o(def);
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {4};  // sigma_y
    req.krylovTrigger = 1e300;  // disable refinement
    req.asymmetryThreshold = 0;
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    ValidationOptions opt;
    validateAgainstReruns(o, req, s, opt, ctl);
    CHECK(s.quality.at(DerivativeSpec::pure(4, 1)).status == QualityStatus::Failed);
}
