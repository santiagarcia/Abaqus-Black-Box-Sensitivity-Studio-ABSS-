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
        int used = 0;
        for (const auto& f : rep.frames) used += f.krylovUsed ? 1 : 0;
        CAPTURE(plate);
        CHECK(used > 0);  // plastic increments detected
        ValidationOptions opt;
        opt.tolerance = {1e-4, 1e-3};
        MESSAGE((plate ? "plate" : "truss") << " with elastic oracle tangent, Krylov at " << used << " increments\n"
                                            << validateAgainstReruns(o, req, s, opt, ctl));
        for (int p : req.parameters) {
            CHECK(s.quality.at(DerivativeSpec::pure(p, 1)).status == QualityStatus::Validated);
            CHECK(s.quality.at(DerivativeSpec::pure(p, 2)).status == QualityStatus::Validated);
        }
    }
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
