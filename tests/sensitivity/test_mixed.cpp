#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace abss;

TEST_CASE("Mixed derivative by polarization: analytic d2u2/(dE dF) on the elastic truss") {
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0, 2};  // E, F
    req.maxOrder = 2;
    req.mixedPairs = {{0, 2}};
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    const double E = 200000, A = 2, L = 10;
    const int fU = s.fieldIndex("U");
    for (int i = 1; i <= 4; ++i) {
        const double amp = 0.25 * i;
        const auto* v = s.get(fU, DerivativeSpec{{0, 2}}, i);
        REQUIRE(v);
        CHECK((*v)[2] == doctest::Approx(-amp * L / (2 * E * E * A)).epsilon(1e-6));
    }
    CHECK(s.derivativeLabel("U1", DerivativeSpec{{0, 2}}).find("∂") != std::string::npos);
}

TEST_CASE("Mixed derivative on the J2 plate matches 4-corner full reruns at every increment") {
    synthetic::SyntheticOracle o(synthetic::plateWithHole(6, 6));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0, 2};  // E, sigma_y
    req.maxOrder = 2;
    req.mixedPairs = {{0, 2}};
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    ValidationOptions opt;
    opt.tolerance = {1e-4, 1e-3};
    MESSAGE(validateAgainstReruns(o, req, s, opt, ctl));
    CHECK(s.quality.at(DerivativeSpec{{0, 2}}).status == QualityStatus::Validated);
    CHECK(s.quality.at(DerivativeSpec::pure(0, 2)).status == QualityStatus::Validated);
}
