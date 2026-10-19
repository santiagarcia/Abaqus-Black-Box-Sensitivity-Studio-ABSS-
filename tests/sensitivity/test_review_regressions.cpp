// Regression tests for the independent review (docs/council/vera_review.md, findings F1, F2, F4).
#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace abss;

namespace {
// Per-increment max relative error of dU/da (order 1) against the central FD of full reruns at +-h.
std::vector<double> perFrameError(synthetic::SyntheticOracle& o, ResultStore& s, int p, double h) {
    std::vector<double> d(s.parameters.size(), 0.0);
    std::vector<FrameFields> fp, fm;
    d[static_cast<size_t>(p)] = h;
    o.solvePerturbed(d, &fp);
    d[static_cast<size_t>(p)] = -h;
    o.solvePerturbed(d, &fm);
    std::vector<double> err(s.frames.size(), 0.0);
    const int fU = s.fieldIndex("U");
    for (size_t fr = 1; fr < s.frames.size(); ++fr) {
        const auto* e = s.get(fU, DerivativeSpec::pure(p, 1), static_cast<int>(fr));
        double num = 0, den = 1e-300;
        for (size_t c = 0; c < e->size(); ++c) {
            const double ref = (fp[fr].values[0][c] - fm[fr].values[0][c]) / (2 * h);
            num = std::max(num, std::abs((*e)[c] - ref));
            den = std::max(den, std::abs(ref));
        }
        err[fr] = num / den;
    }
    return err;
}
}  // namespace

TEST_CASE("F1: cantilever L_beam at the DEFAULT order-2 step validates (adaptive re-probe, no hand-tuned step)") {
    synthetic::SyntheticOracle o(synthetic::cantileverBlock(6, 2, 2));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {5};
    req.maxOrder = 2;
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    ValidationOptions opt;
    MESSAGE(validateAgainstReruns(o, req, s, opt, ctl));
    CHECK(s.quality.at(DerivativeSpec::pure(5, 1)).status == QualityStatus::Validated);
    CHECK(s.quality.at(DerivativeSpec::pure(5, 2)).status == QualityStatus::Validated);
}

TEST_CASE("F1/F4: with re-probing disabled, every increment with a wrong order-1 result is flagged") {
    synthetic::SyntheticOracle o(synthetic::cantileverBlock(6, 2, 2));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {5};
    req.asymmetryThreshold = 0;  // disable the remedy: the detector alone must catch the damage
    const double h = 1e-4 * 10.0;
    req.absoluteSteps = {h};
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    const auto err = perFrameError(o, s, 5, h);
    const auto& flagged = s.quality.at(DerivativeSpec::pure(5, 1)).suspectFrames;
    int bad = 0;
    for (size_t fr = 1; fr < err.size(); ++fr)
        if (err[fr] > 1e-3) {
            ++bad;
            CAPTURE(fr);
            CAPTURE(err[fr]);
            // a wrong increment is either flagged itself or inherits history from an earlier flagged increment
            const bool covered = std::any_of(flagged.begin(), flagged.end(), [&](int f) { return f <= static_cast<int>(fr); });
            CHECK(covered);
        }
    MESSAGE("wrong increments without re-probe: " << bad << ", flagged: " << flagged.size());
}

TEST_CASE("F2: an all-NaN derivative series is never Validated") {
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    for (int f = 0; f < static_cast<int>(s.fields.size()); ++f)
        for (int fr = 1; fr < static_cast<int>(s.frames.size()); ++fr) {
            auto v = *s.get(f, DerivativeSpec::pure(0, 1), fr);
            std::fill(v.begin(), v.end(), std::numeric_limits<double>::quiet_NaN());
            s.set(f, DerivativeSpec::pure(0, 1), fr, v);
        }
    ValidationOptions opt;
    validateAgainstReruns(o, req, s, opt, ctl);
    CHECK(s.quality.at(DerivativeSpec::pure(0, 1)).status == QualityStatus::Failed);
}

TEST_CASE("F2: a run cancelled before the first increment is never Validated") {
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    req.maxOrder = 2;
    RunControl ctl;
    ctl.cancel = true;
    const auto rep = runSensitivities(o, u, req, s, ctl);
    CHECK(rep.cancelled);
    ctl.cancel = false;
    ValidationOptions opt;
    validateAgainstReruns(o, req, s, opt, ctl);
    for (int k : {1, 2}) CHECK(s.quality[DerivativeSpec::pure(0, k)].status != QualityStatus::Validated);
}

TEST_CASE("F2: a field filter that matches nothing does not validate") {
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    ValidationOptions opt;
    opt.fields = {"NO_SUCH_FIELD"};
    validateAgainstReruns(o, req, s, opt, ctl);
    CHECK(s.quality.at(DerivativeSpec::pure(0, 1)).status != QualityStatus::Validated);
}

TEST_CASE("No false predictor flag for a parameter without first-order effect (E under pure displacement control)") {
    // Single CPE4 element, every loaded DOF prescribed: the free lateral DOFs do not depend on E (Abaqus J2 run, E at
    // elastic increments 1-2, was falsely flagged before this guard).
    synthetic::SyntheticOracle o(synthetic::singleQuad(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    CHECK(s.quality.at(DerivativeSpec::pure(0, 1)).suspectFrames.empty());
}