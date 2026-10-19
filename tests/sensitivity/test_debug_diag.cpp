#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <doctest/doctest.h>

using namespace abss;

TEST_CASE("diag: per-frame tangent checks on plate" * doctest::skip(true)) {
    synthetic::SyntheticOracle o(synthetic::plateWithHole(6, 6));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    req.maxOrder = 1;
    RunControl ctl;
    auto rep = runSensitivities(o, u, req, s, ctl);
    for (auto& f : rep.frames) MESSAGE("frame " << f.frame << " tangent " << f.tangentCheckError << " commit " << f.commitResidual[0]);
}

TEST_CASE("diag: cantilever L sweep" * doctest::skip(true)) {
    for (double rel : {1e-3, 1e-4, 1e-5, 1e-6}) {
        synthetic::SyntheticOracle o(synthetic::cantileverBlock(6, 2, 2));
        ResultStore s;
        auto u = o.runNominal(s, nullptr);
        SensitivityRequest req;
        req.parameters = {5};
        req.maxOrder = 1;
        req.absoluteSteps = {rel * 10.0};
        RunControl ctl;
        auto rep = runSensitivities(o, u, req, s, ctl);
        std::string fr;
        for (auto& f : rep.frames) fr += " " + std::to_string(f.commitResidual[0]);
        ValidationOptions opt;
        MESSAGE("rel " << rel << " commit:" << fr << "\n" << validateAgainstReruns(o, req, s, opt, ctl));
    }
}

TEST_CASE("diag: plate step sweep" * doctest::skip(true)) {
    for (double rel : {1e-2, 1e-3, 1e-4, 1e-5}) {
        auto def = synthetic::plateWithHole(6, 6);
        def.newtonTol = 1e-14;
        synthetic::SyntheticOracle o(def);
        ResultStore s;
        auto u = o.runNominal(s, nullptr);
        SensitivityRequest req;
        req.parameters = {0, 2, 4};
        req.maxOrder = 2;
        for (int p : req.parameters) req.absoluteSteps.push_back(rel * s.parameters[static_cast<size_t>(p)].nominal);
        RunControl ctl;
        runSensitivities(o, u, req, s, ctl);
        ValidationOptions opt;
        MESSAGE("rel step " << rel << "\n" << validateAgainstReruns(o, req, s, opt, ctl));
    }
}
