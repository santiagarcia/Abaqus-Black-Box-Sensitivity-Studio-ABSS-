#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace abss;

namespace {

struct Run {
    ResultStore store;
    std::unique_ptr<synthetic::SyntheticOracle> oracle;
    std::vector<std::vector<double>> u;
    SensitivityRequest req;
};

Run run(synthetic::ModelDef def, std::vector<int> params, int order, double relStep = 1e-3) {
    Run r;
    r.oracle = std::make_unique<synthetic::SyntheticOracle>(std::move(def));
    r.u = r.oracle->runNominal(r.store, nullptr);
    r.req.parameters = params;
    r.req.maxOrder = order;
    for (int p : params) r.req.absoluteSteps.push_back(relStep * std::abs(r.store.parameters[static_cast<size_t>(p)].nominal));
    RunControl ctl;
    runSensitivities(*r.oracle, r.u, r.req, r.store, ctl);
    return r;
}

double val(const ResultStore& s, const std::string& field, const DerivativeSpec& d, int frame, std::int64_t loc, int comp) {
    const int f = s.fieldIndex(field);
    const auto* v = s.get(f, d, frame);
    REQUIRE(v != nullptr);
    return (*v)[static_cast<size_t>(loc * s.fields[static_cast<size_t>(f)].componentCount() + comp)];
}

}  // namespace

TEST_CASE("Fornberg central stencils") {
    const auto w1 = stencilWeights({-1, 0, 1}, 1);
    CHECK(w1[0] == doctest::Approx(-0.5));
    CHECK(w1[1] == doctest::Approx(0.0));
    CHECK(w1[2] == doctest::Approx(0.5));
    const auto w2 = stencilWeights({-1, 0, 1}, 2);
    CHECK(w2[0] == doctest::Approx(1.0));
    CHECK(w2[1] == doctest::Approx(-2.0));
    const auto w3 = stencilWeights({-2, -1, 0, 1, 2}, 3);
    CHECK(w3[0] == doctest::Approx(-0.5));
    CHECK(w3[1] == doctest::Approx(1.0));
    CHECK(w3[3] == doctest::Approx(-1.0));
    CHECK(w3[4] == doctest::Approx(0.5));
}

TEST_CASE("Two-bar truss: analytic first/second/third-order displacement sensitivities, all increments") {
    // u2 = F_i L / (2 E A) + g_i / 2 ; node 2 (index 1) direction 1.
    auto r = run(synthetic::twoBarTruss(false), {0, 2, 3}, 3);
    const double E = 200000, A = 2, L = 10, F = 1000;
    for (int i = 1; i <= 4; ++i) {
        const double amp = 0.25 * i, Fi = amp * F;
        const double d1 = -Fi * L / (2 * E * E * A);
        const double d2 = Fi * L / (E * E * E * A);
        const double d3 = -3 * Fi * L / (E * E * E * E * A);
        CHECK(val(r.store, "U", DerivativeSpec::pure(0, 1), i, 1, 0) == doctest::Approx(d1).epsilon(1e-6));
        CHECK(val(r.store, "U", DerivativeSpec::pure(0, 2), i, 1, 0) == doctest::Approx(d2).epsilon(1e-5));
        CHECK(val(r.store, "U", DerivativeSpec::pure(0, 3), i, 1, 0) == doctest::Approx(d3).epsilon(1e-3));
        // du2/dF = L/(2EA), du2/dg = 1/2 (parameter-dependent prescribed BC), and dU3/dg = amp (prescribed DOF).
        CHECK(val(r.store, "U", DerivativeSpec::pure(2, 1), i, 1, 0) == doctest::Approx(amp * L / (2 * E * A)).epsilon(1e-8));
        CHECK(val(r.store, "U", DerivativeSpec::pure(3, 1), i, 1, 0) == doctest::Approx(amp * 0.5).epsilon(1e-8));
        CHECK(val(r.store, "U", DerivativeSpec::pure(3, 1), i, 2, 0) == doctest::Approx(amp).epsilon(1e-10));
        CHECK(std::abs(val(r.store, "U", DerivativeSpec::pure(3, 2), i, 1, 0)) < 1e-6);
        // Reaction at the fixed node: RF1(node1) = -k u2  ->  d/dg = -(EA/L) amp/2
        CHECK(val(r.store, "RF", DerivativeSpec::pure(3, 1), i, 0, 0) == doctest::Approx(-(E * A / L) * amp / 2).epsilon(1e-6));
    }
}

TEST_CASE("Elastic-plastic truss with unloading: order 1 and 2 agree with full reruns at every increment") {
    auto r = run(synthetic::twoBarTruss(true), {0, 4, 5}, 2);
    ValidationOptions opt;
    RunControl ctl;
    const auto summary = validateAgainstReruns(*r.oracle, r.req, r.store, opt, ctl);
    MESSAGE(summary);
    for (int p : {0, 4, 5}) {
        CHECK(r.store.quality[DerivativeSpec::pure(p, 1)].status == QualityStatus::Validated);
        CHECK(r.store.quality[DerivativeSpec::pure(p, 1)].fdRelativeError < 1e-4);
    }
}

TEST_CASE("Plate with hole (J2, load/unload/reload): engine vs full reruns, U/S/PEEQ/Mises, orders 1-2") {
    auto r = run(synthetic::plateWithHole(6, 6), {0, 2, 4, 6}, 2, 1e-4);
    ValidationOptions opt;
    opt.tolerance = {1e-4, 1e-3};
    RunControl ctl;
    const auto summary = validateAgainstReruns(*r.oracle, r.req, r.store, opt, ctl);
    MESSAGE(summary);
    for (int p : {0, 2, 4, 6}) CHECK(r.store.quality[DerivativeSpec::pure(p, 1)].fdRelativeError < 1e-5);
    for (const auto& d : r.req.parameters) CHECK(r.store.quality[DerivativeSpec::pure(d, 1)].tangentCheckError < 1e-5);
}

TEST_CASE("Residual linear in the parameter: first-order result is exact for any step (no truncation error)") {
    // r = F - (E A / L) B u is linear in E, so the frozen-displacement pseudoload is exact even though u(E) ~ 1/E.
    const double E = 200000, A = 2, L = 10, F = 1000;
    const double exact = -F * L / (2 * E * E * A);
    for (double rel : {1e-1, 1e-2, 1e-4}) {
        auto r = run(synthetic::twoBarTruss(false), {0}, 1, rel);
        const double err = std::abs(val(r.store, "U", DerivativeSpec::pure(0, 1), 4, 1, 0) - exact) / std::abs(exact);
        CHECK(err < 1e-9);
    }
}

TEST_CASE("Hardening-modulus sensitivity on the plastic truss converges O(h^2) toward the rerun reference") {
    // Residual is nonlinear in H through the return map -> genuine truncation error that must shrink ~h^2.
    auto ref = run(synthetic::twoBarTruss(true), {5}, 1, 1e-5);
    const double sref = val(ref.store, "U", DerivativeSpec::pure(5, 1), 6, 1, 0);
    double prev = -1;
    for (double rel : {2e-1, 1e-1, 5e-2}) {
        auto r = run(synthetic::twoBarTruss(true), {5}, 1, rel);
        const double err = std::abs(val(r.store, "U", DerivativeSpec::pure(5, 1), 6, 1, 0) - sref) / std::abs(sref);
        MESSAGE("h/H=" << rel << " rel.err=" << err);
        if (prev > 0) CHECK(err < prev / 2.5);
        prev = err;
    }
}

