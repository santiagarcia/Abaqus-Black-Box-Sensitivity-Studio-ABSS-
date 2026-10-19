// Vera (independent reviewer) falsification probes for the ABSS engine/validation.
// Not part of the product build. Links against the already-built build-core libraries; see build_and_run.ps1.
// Each experiment prints evidence; none of them modifies product code.
#include "abss/core/project_io.hpp"
#include "abss/oracle/synthetic.hpp"
#include "abss/sensitivity/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>

using namespace abss;

static const char* st(QualityStatus q) { return qualityName(q); }

// ---- E0: decode the real-Abaqus truss project produced by the builder's run (read-only) -------------------------
static void e0(const std::string& path) {
    std::printf("\n== E0: real-Abaqus truss project %s\n", path.c_str());
    try {
        const auto s = loadProject(path);
        for (const auto& p : s.parameters) (void)p;
        const int fS = s.fieldIndex("S"), fU = s.fieldIndex("U");
        for (const auto& [spec, q] : s.quality) {
            std::string lab;
            for (int p : spec.params) lab += s.parameters[static_cast<size_t>(p)].name + " ";
            const auto* vS = s.get(fS, spec, 1);
            const auto* vU = s.get(fU, spec, 1);
            std::printf("  [%s] status=%s fdErr=%.3g  S'(frame1)=%s  U(node2,1)=%s\n", lab.c_str(), st(q.status),
                        q.fdRelativeError, vS ? std::to_string((*vS)[0]).c_str() : "-",
                        vU ? std::to_string((*vU)[2]).c_str() : "-");
            if (vS) std::printf("      S series exact: %.17g\n", (*vS)[0]);
        }
        const auto* s0 = s.get(fS, DerivativeSpec::nominal(), 1);
        if (s0) std::printf("  nominal S(frame1) = %.17g\n", (*s0)[0]);
    } catch (const std::exception& e) {
        std::printf("  could not load: %s\n", e.what());
    }
}

// ---- E1: NaN in an engine series -> does validation notice? -------------------------------------------------------
static void e1() {
    std::printf("\n== E1: NaN injected into an engine series before validation\n");
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    req.maxOrder = 1;
    RunControl ctl;
    runSensitivities(o, u, req, s, ctl);
    const int fU = s.fieldIndex("U");
    for (int fr = 1; fr < static_cast<int>(s.frames.size()); ++fr) {
        auto v = *s.get(fU, DerivativeSpec::pure(0, 1), fr);
        for (double& x : v) x = std::numeric_limits<double>::quiet_NaN();
        s.set(fU, DerivativeSpec::pure(0, 1), fr, v);  // the whole dU/dE series is NaN
    }
    ValidationOptions opt;
    const auto sum = validateAgainstReruns(o, req, s, opt, ctl);
    std::printf("%s", sum.c_str());
    std::printf("  -> status of dU/dE (all NaN): %s, fdErr=%g\n", st(s.quality[DerivativeSpec::pure(0, 1)].status),
                s.quality[DerivativeSpec::pure(0, 1)].fdRelativeError);
    // Restrict validation to the U field only: does an all-NaN field validate?
    ResultStore s2 = s;
    opt.fields = {"U"};
    const auto sum2 = validateAgainstReruns(o, req, s2, opt, ctl);
    std::printf("  U-only validation: %s", sum2.c_str());
    std::printf("  -> status: %s\n", st(s2.quality[DerivativeSpec::pure(0, 1)].status));
}

// ---- E2: validation of a run that produced no frames (cancelled immediately) -------------------------------------
static void e2() {
    std::printf("\n== E2: engine cancelled before increment 1, then validated\n");
    synthetic::SyntheticOracle o(synthetic::twoBarTruss(false));
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {0};
    req.maxOrder = 2;
    RunControl ctl;
    ctl.cancel = true;
    const auto rep = runSensitivities(o, u, req, s, ctl);
    std::printf("  engine frames computed: %zu, cancelled=%d, series present: %d\n", rep.frames.size(), rep.cancelled,
                s.has(s.fieldIndex("U"), DerivativeSpec::pure(0, 1)));
    RunControl ctl2;
    ValidationOptions opt;
    std::printf("%s", validateAgainstReruns(o, req, s, opt, ctl2).c_str());
    std::printf("  -> status order1=%s order2=%s\n", st(s.quality[DerivativeSpec::pure(0, 1)].status),
                st(s.quality[DerivativeSpec::pure(0, 2)].status));
}

// ---- per-frame comparison helpers --------------------------------------------------------------------------------
struct Ref {
    std::vector<FrameFields> p, m, n;
};
static Ref reruns(Oracle& o, size_t nAll, int pi, double h) {
    Ref r;
    std::vector<double> d(nAll, 0.0);
    o.solvePerturbed(d, &r.n);
    d[static_cast<size_t>(pi)] = h;
    o.solvePerturbed(d, &r.p);
    d[static_cast<size_t>(pi)] = -h;
    o.solvePerturbed(d, &r.m);
    return r;
}
// per-frame max over fields of |a-b| / fieldScale (fieldScale = max over frames |b| floored as the product does)
static std::vector<double> perFrameErr(const ResultStore& s, int pi, const Ref& r, double h, const Ref* r2, double h2) {
    const size_t nF = r.n.size();
    std::vector<double> out(nF, 0.0);
    const double a = std::abs(s.parameters[static_cast<size_t>(pi)].nominal);
    for (size_t f = 0; f < s.fields.size(); ++f) {
        double nomMax = 0, refMax = 0;
        for (size_t fr = 1; fr < nF; ++fr)
            for (size_t c = 0; c < r.n[fr].values[f].size(); ++c) {
                nomMax = std::max(nomMax, std::abs(r.n[fr].values[f][c]));
                refMax = std::max(refMax, std::abs((r.p[fr].values[f][c] - r.m[fr].values[f][c]) / (2 * h)));
            }
        const double scale = std::max(refMax, 1e-4 * nomMax / a);
        for (size_t fr = 1; fr < nF; ++fr) {
            const auto* eng = s.get(static_cast<int>(f), DerivativeSpec::pure(pi, 1), static_cast<int>(fr));
            for (size_t c = 0; c < r.n[fr].values[f].size(); ++c) {
                const double ref = (r.p[fr].values[f][c] - r.m[fr].values[f][c]) / (2 * h);
                double other;
                if (r2) other = (r2->p[fr].values[f][c] - r2->m[fr].values[f][c]) / (2 * h2);
                else other = eng ? (*eng)[c] : 0.0;
                out[fr] = std::max(out[fr], std::abs(other - ref) / scale);
            }
        }
    }
    return out;
}

// ---- E3/E4: detector vs. actual non-smoothness ---------------------------------------------------------------------
static void detectorCase(const char* name, synthetic::ModelDef def, int pi, double rel) {
    synthetic::SyntheticOracle o(def);
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    const double a = std::abs(s.parameters[static_cast<size_t>(pi)].nominal);
    const double h = rel * a;
    SensitivityRequest req;
    req.parameters = {pi};
    req.maxOrder = 1;
    req.absoluteSteps = {h};
    RunControl ctl;
    const auto rep = runSensitivities(o, u, req, s, ctl);
    const auto& q = s.quality[DerivativeSpec::pure(pi, 1)];
    const auto R = reruns(o, s.parameters.size(), pi, h);
    const auto R10 = reruns(o, s.parameters.size(), pi, h / 10);
    const auto eVsRef = perFrameErr(s, pi, R, h, nullptr, 0);   // engine vs rerun FD at the same h (what validation measures)
    const auto kink = perFrameErr(s, pi, R, h, &R10, h / 10);   // rerun FD(h/10) vs rerun FD(h): step dependence
    std::string flagged;
    for (int f : q.suspectFrames) flagged += std::to_string(f) + " ";
    double worstE = 0, worstK = 0;
    std::string missed;
    for (size_t fr = 1; fr < kink.size(); ++fr) {
        worstE = std::max(worstE, eVsRef[fr]);
        worstK = std::max(worstK, kink[fr]);
        const bool isFlag = std::find(q.suspectFrames.begin(), q.suspectFrames.end(), static_cast<int>(fr)) != q.suspectFrames.end();
        if (kink[fr] > 1e-2 && !isFlag) missed += std::to_string(fr) + " ";
    }
    // validation result as the product computes it
    ValidationOptions opt;
    opt.tolerance = {1e-4, 1e-3};
    validateAgainstReruns(o, req, s, opt, ctl);
    std::printf("  %-10s %-8s h/a=%-7g flagged=[%s] maxErr(engine vs FD_h)=%.2e maxStepDep(FD_h vs FD_h/10)=%.2e "
                "UNFLAGGED frames with step-dependence>1e-2: [%s] status=%s\n",
                name, s.parameters[static_cast<size_t>(pi)].name.c_str(), rel, flagged.c_str(), worstE, worstK, missed.c_str(),
                st(s.quality[DerivativeSpec::pure(pi, 1)].status));
    if (!missed.empty() || std::string(name) == "cantilev" ) {
        std::printf("     per-frame: fr  stepDep   engVsFD  commitRes  asym    reprobe(conv)\n");
        for (const auto& fd : rep.frames)
            std::printf("               %2d  %.2e  %.2e  %.2e  %.2e  %d(%d)\n", fd.frame, kink[static_cast<size_t>(fd.frame)],
                        eVsRef[static_cast<size_t>(fd.frame)], fd.commitResidual[0], fd.asymmetry[0], fd.reprobePasses[0],
                        static_cast<int>(fd.reprobeConverged[0]));
    }
}

static void e34() {
    std::printf("\n== E3/E4: non-smoothness detector vs. step-dependence of full-rerun FD (kink proxy)\n");
    for (double rel : {1e-2, 3e-3, 1e-3, 3e-4, 1e-4}) {
        detectorCase("cantilev", synthetic::cantileverBlock(6, 2, 2), 2, rel);  // sigma_y (minus branch yields first)
        detectorCase("cantilev", synthetic::cantileverBlock(6, 2, 2), 5, rel);  // L_beam (plus branch yields first)
        detectorCase("plate", synthetic::plateWithHole(6, 6), 2, rel);          // sigma_y
        detectorCase("plate", synthetic::plateWithHole(6, 6), 0, rel);          // E
        detectorCase("truss", synthetic::twoBarTruss(true), 4, rel);            // sigma_y
    }
}

// ---- E5: GMRES starved of iterations: is it flagged anywhere? -----------------------------------------------------
static void e5() {
    std::printf("\n== E5: matrix-free GMRES forced to stop early (krylovMaxIterations=1, tol=1e-9)\n");
    auto def = synthetic::plateWithHole(6, 6);
    def.reportElasticTangent = true;
    synthetic::SyntheticOracle o(def);
    ResultStore s;
    auto u = o.runNominal(s, nullptr);
    SensitivityRequest req;
    req.parameters = {2};
    req.maxOrder = 1;
    req.krylovMaxIterations = 1;
    RunControl ctl;
    const auto rep = runSensitivities(o, u, req, s, ctl);
    double worst = 0;
    for (const auto& f : rep.frames) worst = std::max(worst, f.krylovResidual);
    const auto& q = s.quality[DerivativeSpec::pure(2, 1)];
    std::printf("  worst GMRES relative residual=%.3g; suspectFrames=%zu; status before validation=%s\n", worst,
                q.suspectFrames.size(), st(q.status));
    for (const auto& n : q.notes) std::printf("   note: %s\n", n.c_str());
    ValidationOptions opt;
    std::printf("%s", validateAgainstReruns(o, req, s, opt, ctl).c_str());
}

// ---- E6: silent O(1) engine error on a smooth response (cantilever L_beam), default steps, re-probe threshold -------
static void e6() {
    std::printf("\n== E6: cantilever L_beam, engine DEFAULT steps (no absoluteSteps), varying asymmetryThreshold / passes\n");
    struct Cfg {
        int K;
        double thr;
        int passes;
    };
    for (Cfg c : {Cfg{1, 0.05, 0}, Cfg{2, 0.05, 0}, Cfg{2, 0.01, 0}, Cfg{2, 0.001, 0}, Cfg{2, 0.05, 3}, Cfg{2, 0.05, 10}}) {
        synthetic::SyntheticOracle o(synthetic::cantileverBlock(6, 2, 2));
        ResultStore s;
        auto u = o.runNominal(s, nullptr);
        SensitivityRequest req;
        req.parameters = {5};
        req.maxOrder = c.K;
        req.asymmetryThreshold = c.thr;
        req.defectCorrectionPasses = c.passes;
        RunControl ctl;
        const auto rep = runSensitivities(o, u, req, s, ctl);
        const auto q1 = s.quality[DerivativeSpec::pure(5, 1)];
        int rp = 0;
        for (const auto& f : rep.frames) rp += f.reprobePasses[0];
        ValidationOptions opt;
        const auto sum = validateAgainstReruns(o, req, s, opt, ctl);
        std::printf("  K=%d asymThr=%-6g forcedPasses=%-2d -> suspectFrames(before validation)=%zu reprobePasses=%d | %s", c.K, c.thr,
                    c.passes, q1.suspectFrames.size(), rp, sum.c_str());
    }
}

int main(int argc, char** argv) {
    if (argc > 1) e0(argv[1]);
    e1();
    e2();
    e5();
    e6();
    if (argc > 2) e34();
    return 0;
}
