#include "abss/sensitivity/engine.hpp"

#include "abss/core/profile.hpp"
#include "abss/linalg/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace abss {

namespace {

double factorial(int k) {
    double f = 1;
    for (int i = 2; i <= k; ++i) f *= i;
    return f;
}

std::string fmt(double v) {
    std::ostringstream os;
    os.precision(3);
    os << v;
    return os.str();
}

template <class F>
void forEachMaybeParallel(size_t n, bool parallel, F&& f) {
    if (!parallel || n < 2) {
        for (size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::vector<std::future<void>> fut;
    for (size_t i = 0; i < n; ++i) fut.push_back(std::async(std::launch::async, [&f, i] { f(i); }));
    for (auto& x : fut) x.get();
}

double maxAbs(const std::vector<double>& v) {
    double m = 0;
    for (double x : v) m = std::max(m, std::abs(x));
    return m;
}

/// A perturbation direction in parameter space. All internal derivatives are taken w.r.t. the scalar t, where the
/// parameters are a(t) = a* + t v (so a unit step in t is the user's finite-difference step).
struct Direction {
    std::vector<double> v;  ///< full-length parameter offset per unit t
    int pure = -1;          ///< parameter index for pure directions
    double hPure = 0;       ///< |v| along `pure`
    double hRel = 0;        ///< largest relative step |v_p| / scale_p of the direction
    int mixA = -1, mixB = -1;
    int sign = 0;           ///< +1: a+b, -1: a-b
    std::string label;
};

}  // namespace

SensitivityReport runSensitivities(Oracle& oracle, const std::vector<std::vector<double>>& uNominal,
                                   const SensitivityRequest& req, ResultStore& store, RunControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    SensitivityReport rep;
    rep.computeBackend = par::backendDescription();
    const int K = req.maxOrder;
    if (K < 1) throw std::invalid_argument("maxOrder must be >= 1");
    if (!req.mixedPairs.empty() && K < 2) throw std::invalid_argument("mixed derivatives need maxOrder >= 2");
    const int M = req.halfWidth > 0 ? req.halfWidth : defaultHalfWidth(K);
    if (2 * M + 1 < K + 1) throw std::invalid_argument("stencil too narrow for the requested order");
    const auto& dm = oracle.dofMap();
    const size_t ndof = static_cast<size_t>(dm.size());
    const int nFrames = static_cast<int>(uNominal.size()) - 1;
    const size_t nAll = store.parameters.size();
    if (nFrames < 1) throw std::invalid_argument("no increments to differentiate");

    std::vector<int> offsets;
    for (int m = -M; m <= M; ++m) offsets.push_back(m);
    std::vector<std::vector<double>> w(static_cast<size_t>(K) + 1);
    for (int k = 1; k <= K; ++k) w[static_cast<size_t>(k)] = stencilWeights(offsets, k);
    std::vector<int> off;
    for (int m : offsets)
        if (m != 0) off.push_back(m);
    auto wAt = [&](int k, int m) { return w[static_cast<size_t>(k)][static_cast<size_t>(m + M)]; };
    const size_t bPlus = static_cast<size_t>(std::find(off.begin(), off.end(), 1) - off.begin());
    const size_t bMinus = static_cast<size_t>(std::find(off.begin(), off.end(), -1) - off.begin());

    // ---- directions -----------------------------------------------------------------------------------------
    std::vector<double> hOf(nAll, 0.0);
    std::vector<Direction> dirs;
    for (size_t j = 0; j < req.parameters.size(); ++j) {
        const int pi = req.parameters[j];
        const auto& p = store.parameters.at(static_cast<size_t>(pi));
        const double h = j < req.absoluteSteps.size() && req.absoluteSteps[j] > 0
                             ? req.absoluteSteps[j]
                             : defaultRelativeStep(K) * (p.absoluteStep() / p.relativeStep);  // |nominal|, or bound-based scale when 0
        if (p.mustBePositive && p.nominal - M * h <= 0)
            throw std::invalid_argument("Perturbation of '" + p.name + "' would make it non-positive; reduce the step");
        if (p.nominal - M * h < p.lowerBound || p.nominal + M * h > p.upperBound)
            throw std::invalid_argument("Perturbation of '" + p.name + "' leaves its allowed range; reduce the step");
        hOf[static_cast<size_t>(pi)] = h;
        Direction d;
        d.v.assign(nAll, 0.0);
        d.v[static_cast<size_t>(pi)] = h;
        d.pure = pi;
        d.hPure = h;
        d.hRel = h / (p.absoluteStep() / p.relativeStep);
        d.label = p.name;
        dirs.push_back(std::move(d));
    }
    for (const auto& [a, b] : req.mixedPairs) {
        if (a == b || hOf.at(static_cast<size_t>(a)) == 0 || hOf.at(static_cast<size_t>(b)) == 0)
            throw std::invalid_argument("mixed pair parameters must be distinct and also requested as parameters");
        for (int s : {+1, -1}) {
            Direction d;
            d.v.assign(nAll, 0.0);
            d.v[static_cast<size_t>(a)] = hOf[static_cast<size_t>(a)];
            d.v[static_cast<size_t>(b)] = s * hOf[static_cast<size_t>(b)];
            d.mixA = std::min(a, b);
            d.mixB = std::max(a, b);
            d.sign = s;
            for (int pp : {a, b}) {
                const auto& par = store.parameters[static_cast<size_t>(pp)];
                d.hRel = std::max(d.hRel, hOf[static_cast<size_t>(pp)] / (par.absoluteStep() / par.relativeStep));
            }
            d.label = store.parameters[static_cast<size_t>(a)].name + (s > 0 ? "+" : "-") + store.parameters[static_cast<size_t>(b)].name;
            dirs.push_back(std::move(d));
        }
    }
    const size_t nD = dirs.size();

    auto log = [&](const std::string& s) {
        store.log.push_back(s);
        if (ctl.log) ctl.log(s);
    };
    log("Sensitivity run: order " + std::to_string(K) + ", " + std::to_string(req.parameters.size()) + " parameter(s), " +
        std::to_string(req.mixedPairs.size()) + " mixed pair(s), " + std::to_string(nD) + " direction(s) x " +
        std::to_string(off.size()) + " history branches + 1 nominal replay; compute backend: " + rep.computeBackend);

    std::atomic<long long> evals{0};
    auto base = oracle.openBranch(std::vector<double>(nAll, 0.0));
    std::vector<std::vector<std::unique_ptr<HistoryBranch>>> br(nD);
    for (size_t d = 0; d < nD; ++d)
        for (int m : off) {
            std::vector<double> delta(nAll);
            for (size_t q = 0; q < nAll; ++q) delta[q] = m * dirs[d].v[q];
            br[d].push_back(oracle.openBranch(delta));
        }

    const size_t nFields = store.fields.size();
    double maxTangentErr = -1.0;
    std::vector<double> maxCommit(nD, 0.0), maxLeak(nD, 0.0);
    std::vector<std::vector<int>> suspicious(nD);
    std::vector<std::vector<std::string>> engineIssues(nD);
    double worstReplay = 0;           // max over increments of |r0 at free DOFs| / |reactions| (nominal replay)
    std::vector<int> replayBad;       // increments where the replay does not reproduce equilibrium
    // Residuals at u*_{i+1} evaluated by the commit jobs of increment i (HistoryBranch::commitAndProbe).
    std::vector<std::vector<std::vector<double>>> fusedProbe(nD, std::vector<std::vector<double>>(off.size()));
    std::vector<std::vector<char>> haveFused(nD, std::vector<char>(off.size(), 0));
    double gmresMatvecsPerRhs = 0;  // measured on the latest GMRES increment (cost model for the assembled tangent)
    std::vector<std::vector<double>> fusedBase;  // replay-branch residuals at baseProbeStates(i + 1), in that order
    // States probed on the replay branch at increment j: u*_j (replay check, r0) and, with the tangent check, the pair
    // u*_j +- eps v (v fixed, eps relative to max |u*_j| on the free DOFs of increment j).
    auto baseProbeStates = [&](int j, std::vector<double>& v, double& eps) {
        const auto Fj = oracle.freeIndicesAt(j);
        const auto& u = uNominal[static_cast<size_t>(j)];
        std::vector<std::vector<double>> s{u};
        if (!req.tangentCheck) return s;
        v.assign(Fj.size(), 0.0);
        for (size_t q = 0; q < Fj.size(); ++q) v[q] = std::sin(1.0 + 0.7 * double(q)) + 0.5;
        double uMax = 1e-12;
        for (auto f : Fj) uMax = std::max(uMax, std::abs(u[static_cast<size_t>(f)]));
        eps = 1e-6 * uMax;
        std::vector<double> up = u, um = u;
        for (size_t q = 0; q < Fj.size(); ++q) {
            up[static_cast<size_t>(Fj[q])] += eps * v[q];
            um[static_cast<size_t>(Fj[q])] -= eps * v[q];
        }
        s.push_back(std::move(up));
        s.push_back(std::move(um));
        return s;
    };

    for (int i = 1; i <= nFrames; ++i) {
        if (ctl.cancel) {
            rep.cancelled = true;
            log("Cancelled before increment " + std::to_string(i) + "; completed increments are kept.");
            break;
        }
        FrameDiagnostics diag;
        diag.frame = i;
        const auto F = oracle.freeIndicesAt(i);  // unknowns of this increment (BC sets may change between steps)
        const auto& uS = uNominal[static_cast<size_t>(i)];
        // wk[d][k]: k-th derivative of u w.r.t. t along direction d (full length; free entries meaningful).
        std::vector<std::vector<std::vector<double>>> wk(nD, std::vector<std::vector<double>>(static_cast<size_t>(K) + 1));
        auto predictor = [&](size_t d, int m, int upTo) {
            std::vector<double> u = uS;
            for (int l = 1; l <= upTo; ++l) {
                const double c = std::pow(double(m), l) / factorial(l);
                const auto& x = wk[d][static_cast<size_t>(l)];
                for (auto f : F) u[static_cast<size_t>(f)] += c * x[static_cast<size_t>(f)];
            }
            return u;
        };
        auto probeAll = [&](int upTo, const std::vector<size_t>& which, const char* activity) {
            prof::Scope ws(std::string("stage.") + activity);
            std::vector<std::vector<std::vector<double>>> r(nD, std::vector<std::vector<double>>(off.size()));
            std::vector<WaveRequest> wave;
            for (size_t d : which)
                for (size_t b = 0; b < off.size(); ++b) {
                    WaveRequest w;
                    w.branch = br[d][b].get();
                    w.activity = std::string("probe.") + activity;
                    w.probes = {predictor(d, off[b], upTo)};
                    wave.push_back(std::move(w));
                }
            oracle.runWave(i, wave, req.parallelBranches);
            size_t t = 0;
            for (size_t d : which)
                for (size_t b = 0; b < off.size(); ++b) r[d][b] = std::move(wave[t++].residuals.at(0));
            evals += static_cast<long long>(wave.size());
            return r;
        };
        std::vector<size_t> allDirs(nD);
        for (size_t d = 0; d < nD; ++d) allDirs[d] = d;

        // Probe wave: every evaluation of this increment that needs no solve runs as one concurrent wave (the oracle's
        // job slots bound the concurrency): the replay probe r0 = g(0) (measured, never assumed zero), the tangent-check
        // pair u* +- eps v, and the order-1 branch probes (their predictor is u* itself).
        std::vector<double> vTC;
        double epsTC = 0;
        auto baseUs = baseProbeStates(i, vTC, epsTC);
        // Residuals that the previous increment's commit jobs already evaluated on exactly this history
        // (commitAndProbe) are used as they are; only the rest is probed.
        std::vector<std::vector<double>> baseProbes;  // [0] = r0, [1], [2] = tangent-check pair (when requested)
        std::vector<std::vector<std::vector<double>>> firstBranchProbes(nD, std::vector<std::vector<double>>(off.size()));
        const size_t baseReused = std::min(fusedBase.size(), baseUs.size());
        {
            prof::Scope ws("stage.probe_wave");
            baseUs.erase(baseUs.begin(), baseUs.begin() + static_cast<std::ptrdiff_t>(baseReused));
            std::vector<WaveRequest> wave;
            if (!baseUs.empty()) {
                WaveRequest w;
                w.branch = base.get();
                w.activity = baseReused ? "probe.tangent_check" : req.tangentCheck ? "probe.replay_and_tangent_check"
                                                                                   : "probe.replay";
                w.probes = baseUs;
                wave.push_back(std::move(w));
            }
            const bool baseInWave = !wave.empty();
            std::vector<std::pair<size_t, size_t>> todo;
            for (size_t d = 0; d < nD; ++d)
                for (size_t b = 0; b < off.size(); ++b) {
                    if (haveFused[d][b]) {
                        firstBranchProbes[d][b] = std::move(fusedProbe[d][b]);
                        continue;
                    }
                    todo.emplace_back(d, b);
                    WaveRequest w;
                    w.branch = br[d][b].get();
                    w.activity = "probe.branch";
                    w.probes = {predictor(d, off[b], 0)};
                    wave.push_back(std::move(w));
                }
            if ((req.parallelBranches && req.overlapWaves) || oracle.batchesWaves()) {
                oracle.runWave(i, wave, req.parallelBranches);
            } else if (!wave.empty()) {  // earlier order: the replay branch first, then the perturbed branches
                std::vector<WaveRequest> first(std::make_move_iterator(wave.begin()), std::make_move_iterator(wave.begin() + 1));
                std::vector<WaveRequest> rest(std::make_move_iterator(wave.begin() + 1), std::make_move_iterator(wave.end()));
                oracle.runWave(i, first, false);
                oracle.runWave(i, rest, req.parallelBranches);
                wave.clear();
                wave.push_back(std::move(first[0]));
                for (auto& w : rest) wave.push_back(std::move(w));
            }
            if (baseInWave) {
                baseProbes = std::move(wave[0].residuals);
                evals += static_cast<long long>(baseUs.size());
            }
            for (size_t t = 0; t < todo.size(); ++t) {
                const auto [d, b] = todo[t];
                firstBranchProbes[d][b] = std::move(wave[t + (baseInWave ? 1 : 0)].residuals.at(0));
                ++evals;
            }
            const long long reused = static_cast<long long>(nD * off.size() - todo.size() + baseReused);
            if (reused) prof::add("engine.fused_probes_used", 0.0, reused);
        }
        baseProbes.insert(baseProbes.begin(), std::make_move_iterator(fusedBase.begin()),
                          std::make_move_iterator(fusedBase.begin() + static_cast<std::ptrdiff_t>(baseReused)));
        fusedBase.clear();
        const std::vector<double> r0 = baseProbes.at(0);
        {
            // Replay check: at the converged nominal state the free-DOF residual must vanish (to the nominal solver
            // tolerance). A large value means the oracle does not reproduce this analysis (unsupported semantics).
            std::vector<char> isFree(ndof, 0);
            for (auto f : F) isFree[static_cast<size_t>(f)] = 1;
            double rf = 0, rp = 0;
            for (size_t q = 0; q < ndof; ++q) (isFree[q] ? rf : rp) = std::max(isFree[q] ? rf : rp, std::abs(r0[q]));
            if (rp > 0) {
                worstReplay = std::max(worstReplay, rf / rp);
                if (rf / rp > 1e-2) replayBad.push_back(i);
            }
        }

        CsrMatrix KFF;
        {
            prof::Scope s("engine.tangent_assembly");
            KFF = oracle.tangent(i).submatrix(F, F);
        }
        SolveReport srep;
        std::unique_ptr<Factorization> fact;
        {
            prof::Scope s("engine.factorize");
            fact = factorize(KFF, req.solver, &srep);
        }
        if (!fact->ok()) throw std::runtime_error("Tangent factorization failed at increment " + std::to_string(i) + ": " + fact->message());
        diag.solver = srep.message;

        bool useKrylov = req.forceKrylov;
        int gmresRhsThisIncrement = 0;
        std::mutex diagMutex, factMutex;
        double uScale = 1e-12;
        for (auto f : F) uScale = std::max(uScale, std::abs(uS[static_cast<size_t>(f)]));
        // K_true v by central differences of frozen-history probes on the nominal replay branch.
        auto applyTrue = [&](const std::vector<double>& vF) {
            std::vector<double> out(F.size(), 0.0);
            const double vmax = maxAbs(vF);
            if (vmax == 0) return out;
            const double eps = 1e-6 * uScale / vmax;
            std::vector<double> up = uS, um = uS;
            for (size_t q = 0; q < F.size(); ++q) {
                up[static_cast<size_t>(F[q])] += eps * vF[q];
                um[static_cast<size_t>(F[q])] -= eps * vF[q];
            }
            prof::Scope s("stage.gmres_matvec");
            prof::Activity act("probe.gmres_matvec");
            const auto rr = base->residuals(i, {up, um});
            evals += 2;
            for (size_t q = 0; q < F.size(); ++q)
                out[q] = -(rr[0][static_cast<size_t>(F[q])] - rr[1][static_cast<size_t>(F[q])]) / (2 * eps);
            std::lock_guard<std::mutex> lk(diagMutex);
            diag.krylovMatvecs++;
            return out;
        };
        // Right-preconditioned GMRES(m) with the oracle-tangent factorization as preconditioner. Right-hand sides are
        // independent (same operator), so with concurrent jobs they are solved side by side (solveBlock).
        auto gmres = [&](const std::vector<double>& b) {
            const size_t n = F.size();
            auto prec = [&](std::vector<double> v) {
                std::lock_guard<std::mutex> lk(factMutex);
                fact->solve(v, 1);
                return v;
            };
            auto nrm = [](const std::vector<double>& v) {
                double s = 0;
                for (double x : v) s += x * x;
                return std::sqrt(s);
            };
            const double bn = nrm(b);
            std::vector<double> x = prec(b);
            if (bn == 0) return x;
            double rel = 1.0;
            for (int restart = 0; restart < 4; ++restart) {
                const auto ax = applyTrue(x);
                std::vector<double> r(n);
                for (size_t q = 0; q < n; ++q) r[q] = b[q] - ax[q];
                const double beta = nrm(r);
                rel = beta / bn;
                if (rel <= req.krylovTolerance) break;
                const int m = req.krylovMaxIterations;
                std::vector<std::vector<double>> V{r}, Z;
                for (double& v : V[0]) v /= beta;
                std::vector<std::vector<double>> H(static_cast<size_t>(m) + 1, std::vector<double>(static_cast<size_t>(m), 0.0));
                std::vector<double> cs(static_cast<size_t>(m)), sn(static_cast<size_t>(m)), g(static_cast<size_t>(m) + 1, 0.0);
                g[0] = beta;
                int k = 0;
                for (; k < m; ++k) {
                    Z.push_back(prec(V[static_cast<size_t>(k)]));
                    auto w = applyTrue(Z.back());
                    for (int j = 0; j <= k; ++j) {
                        double h = 0;
                        for (size_t q = 0; q < n; ++q) h += w[q] * V[static_cast<size_t>(j)][q];
                        H[static_cast<size_t>(j)][static_cast<size_t>(k)] = h;
                        for (size_t q = 0; q < n; ++q) w[q] -= h * V[static_cast<size_t>(j)][q];
                    }
                    const double hn = nrm(w);
                    H[static_cast<size_t>(k) + 1][static_cast<size_t>(k)] = hn;
                    for (int j = 0; j < k; ++j) {
                        const double a = H[static_cast<size_t>(j)][static_cast<size_t>(k)], c = H[static_cast<size_t>(j) + 1][static_cast<size_t>(k)];
                        H[static_cast<size_t>(j)][static_cast<size_t>(k)] = cs[static_cast<size_t>(j)] * a + sn[static_cast<size_t>(j)] * c;
                        H[static_cast<size_t>(j) + 1][static_cast<size_t>(k)] = -sn[static_cast<size_t>(j)] * a + cs[static_cast<size_t>(j)] * c;
                    }
                    const double a = H[static_cast<size_t>(k)][static_cast<size_t>(k)], c = H[static_cast<size_t>(k) + 1][static_cast<size_t>(k)];
                    const double d = std::hypot(a, c);
                    cs[static_cast<size_t>(k)] = d > 0 ? a / d : 1.0;
                    sn[static_cast<size_t>(k)] = d > 0 ? c / d : 0.0;
                    H[static_cast<size_t>(k)][static_cast<size_t>(k)] = d;
                    H[static_cast<size_t>(k) + 1][static_cast<size_t>(k)] = 0;
                    g[static_cast<size_t>(k) + 1] = -sn[static_cast<size_t>(k)] * g[static_cast<size_t>(k)];
                    g[static_cast<size_t>(k)] = cs[static_cast<size_t>(k)] * g[static_cast<size_t>(k)];
                    if (std::abs(g[static_cast<size_t>(k) + 1]) / bn <= req.krylovTolerance || hn == 0) {
                        ++k;
                        break;
                    }
                    for (double& v : w) v /= hn;
                    V.push_back(std::move(w));
                }
                std::vector<double> y(static_cast<size_t>(k), 0.0);
                for (int j = k - 1; j >= 0; --j) {
                    double s = g[static_cast<size_t>(j)];
                    for (int l = j + 1; l < k; ++l) s -= H[static_cast<size_t>(j)][static_cast<size_t>(l)] * y[static_cast<size_t>(l)];
                    y[static_cast<size_t>(j)] = s / H[static_cast<size_t>(j)][static_cast<size_t>(j)];
                }
                for (int j = 0; j < k; ++j)
                    for (size_t q = 0; q < n; ++q) x[q] += y[static_cast<size_t>(j)] * Z[static_cast<size_t>(j)][q];
            }
            const auto ax = applyTrue(x);
            double rn = 0;
            for (size_t q = 0; q < n; ++q) rn += (b[q] - ax[q]) * (b[q] - ax[q]);
            std::lock_guard<std::mutex> lk(diagMutex);
            diag.krylovResidual = std::max(diag.krylovResidual, std::sqrt(rn) / bn);
            // The operator is a central difference of oracle probes, so GMRES stagnates at its noise floor (measured
            // ~1e-6 on a large-rotation shell). That floor is far below the derivative accuracy that matters; only a
            // residual that could visibly change the result (> 1e-4) is an engine failure.
            if (std::sqrt(rn) / bn > std::max(10 * req.krylovTolerance, 1e-4)) diag.krylovConverged = false;
            return x;
        };

        auto solveBlock = [&](const std::vector<std::vector<double>>& rhs) {
            if (useKrylov) {
                prof::Scope s("stage.gmres_solve");
                diag.krylovUsed = true;
                std::vector<std::vector<double>> x(rhs.size());
                forEachMaybeParallel(rhs.size(), req.parallelBranches && req.overlapWaves,
                                     [&](size_t c) { x[c] = gmres(rhs[c]); });
                prof::add("engine.gmres_rhs", 0.0, static_cast<long long>(rhs.size()));
                gmresRhsThisIncrement += static_cast<int>(rhs.size());
                return x;
            }
            prof::Scope s("engine.direct_solve");
            prof::add("engine.direct_solve_rhs", 0.0, static_cast<long long>(rhs.size()));
            std::vector<double> block(F.size() * rhs.size());
            for (size_t c = 0; c < rhs.size(); ++c) std::copy(rhs[c].begin(), rhs[c].end(), block.begin() + static_cast<std::ptrdiff_t>(c * F.size()));
            fact->solve(block, static_cast<int>(rhs.size()));
            std::vector<std::vector<double>> x(rhs.size());
            for (size_t c = 0; c < rhs.size(); ++c) {
                x[c].assign(block.begin() + static_cast<std::ptrdiff_t>(c * F.size()), block.begin() + static_cast<std::ptrdiff_t>((c + 1) * F.size()));
                const auto ax = KFF.multiply(x[c]);
                double rn = 0, bn = 0;
                for (size_t q = 0; q < F.size(); ++q) {
                    rn = std::max(rn, std::abs(ax[q] - rhs[c][q]));
                    bn = std::max(bn, std::abs(rhs[c][q]));
                }
                diag.solveResidual = std::max(diag.solveResidual, bn > 0 ? rn / bn : rn);
            }
            return x;
        };

        if (req.tangentCheck) {
            const auto& rr = baseProbes;  // [1] = r(u* + eps v), [2] = r(u* - eps v)
            const auto Kv = KFF.multiply(vTC);
            double num = 0, den = 0;
            for (size_t q = 0; q < F.size(); ++q) {
                const double fd = -(rr[1][static_cast<size_t>(F[q])] - rr[2][static_cast<size_t>(F[q])]) / (2 * epsTC);
                num = std::max(num, std::abs(Kv[q] - fd));
                den = std::max(den, std::abs(Kv[q]));
            }
            diag.tangentCheckError = den > 0 ? num / den : num;
            maxTangentErr = std::max(maxTangentErr, diag.tangentCheckError);
            if (diag.tangentCheckError > req.krylovTrigger) useKrylov = true;
        }
        // Inconsistent tangent: either assemble the true tangent from frozen-history probes, or let GMRES apply it
        // matrix-free. Assembly groups structurally orthogonal columns (Curtis-Powell-Reid colouring of the oracle
        // tangent's pattern): 2 probes per colour, all independent, once per increment, however many directions.
        // GMRES costs ~2 probes per iteration for EVERY right-hand side, iterations in sequence. The choice uses the
        // estimated Abaqus job rounds (job slots = req.concurrency) with the GMRES iteration count measured on earlier
        // increments. The assembled tangent is verified against the tangent-check probe pair; on disagreement (e.g.
        // couplings outside the element pattern) GMRES is used.
        if (useKrylov && !req.forceKrylov) {
            // Grouping needs the complete coupling pattern. Without it (constraints, contact, DOFs without element
            // stiffness) every column is probed on its own and every row is read: exact, but 2 probes per free DOF.
            const bool grouped = oracle.tangentPatternComplete(i);
            std::vector<std::vector<std::int32_t>> pat;
            std::vector<std::int32_t> colour;
            std::int32_t nc = 0;
            if (grouped) {
                pat = symmetricColumnPattern(KFF);
                colour = columnColouring(pat, &nc);
            } else {
                nc = static_cast<std::int32_t>(F.size());
                std::vector<std::int32_t> all(F.size());
                for (size_t q = 0; q < F.size(); ++q) all[q] = static_cast<std::int32_t>(q);
                pat.assign(F.size(), all);
                colour = all;
            }
            const double C = std::max(1, req.concurrency);
            const double ppe = std::max(1, oracle.replayProbesPerEvaluation());  // probes one evaluation holds
            const double rhsCount = double(nD) * K;
            const double itEst = gmresMatvecsPerRhs > 0 ? gmresMatvecsPerRhs : 12.0;
            const bool rhsConcurrent = req.parallelBranches && req.overlapWaves;
            const double perProduct = std::ceil(2.0 / ppe);  // evaluations per GMRES operator application
            const double gmresRounds = itEst * (rhsConcurrent ? std::ceil(perProduct * rhsCount / C) : rhsCount * std::ceil(perProduct / C));
            const double colouredRounds = std::ceil(std::ceil(2.0 * nc / ppe) / C);
            diag.tangentColours = nc;
            if (colouredRounds <= gmresRounds) {
                std::vector<std::vector<double>> us;
                us.reserve(2 * static_cast<size_t>(nc));
                const double eps = 1e-6 * uScale;
                for (std::int32_t c = 0; c < nc; ++c)
                    for (double s : {1.0, -1.0}) {
                        auto u = uS;
                        for (size_t q = 0; q < F.size(); ++q)
                            if (colour[q] == c) u[static_cast<size_t>(F[q])] += s * eps;
                        us.push_back(std::move(u));
                    }
                std::vector<std::vector<double>> rr;
                {
                    prof::Scope ws("stage.tangent_columns");
                    prof::Activity act("probe.tangent_colour");
                    rr = base->residuals(i, us);
                }
                evals += static_cast<long long>(us.size());
                std::vector<Triplet> t;
                for (size_t q = 0; q < F.size(); ++q) {
                    const auto c = static_cast<size_t>(colour[q]);
                    for (auto s : pat[q]) {
                        const auto g = static_cast<size_t>(F[static_cast<size_t>(s)]);
                        const double v = -(rr[2 * c][g] - rr[2 * c + 1][g]) / (2 * eps);
                        if (v != 0.0) t.push_back({s, static_cast<std::int32_t>(q), v});
                    }
                }
                auto Ktrue = CsrMatrix::fromTriplets(static_cast<std::int32_t>(F.size()), static_cast<std::int32_t>(F.size()), std::move(t));
                // Independent check: the assembled tangent must reproduce the tangent-check probe pair ROW BY ROW, each
                // row measured against its own size sum_c |K_qc v_c| (floored at 1e-3 of the largest row, below which
                // finite-difference noise dominates), so that small rows cannot hide errors behind large ones.
                bool agrees = true;
                if (req.tangentCheck) {
                    const auto Kv = Ktrue.multiply(vTC);
                    std::vector<double> rowSize(F.size(), 0.0);
                    for (std::int32_t r = 0; r < Ktrue.rows; ++r)
                        for (auto k = Ktrue.rowPtr[static_cast<size_t>(r)]; k < Ktrue.rowPtr[static_cast<size_t>(r) + 1]; ++k)
                            rowSize[static_cast<size_t>(r)] += std::abs(Ktrue.values[static_cast<size_t>(k)] * vTC[static_cast<size_t>(Ktrue.colIdx[static_cast<size_t>(k)])]);
                    const double floor = 1e-3 * std::max(1e-300, *std::max_element(rowSize.begin(), rowSize.end()));
                    double worst = 0;
                    for (size_t q = 0; q < F.size(); ++q) {
                        const double fd = -(baseProbes[1][static_cast<size_t>(F[q])] - baseProbes[2][static_cast<size_t>(F[q])]) / (2 * epsTC);
                        worst = std::max(worst, std::abs(Kv[q] - fd) / std::max(rowSize[q], floor));
                    }
                    diag.assembledTangentCheck = worst;
                    agrees = diag.assembledTangentCheck <= 1e-4;
                    if (!agrees)
                        log("Increment " + std::to_string(i) + ": tangent assembled from " + std::to_string(us.size()) +
                            " probes disagrees with the check probes (" + fmt(diag.assembledTangentCheck) +
                            "); using GMRES instead.");
                }
                SolveReport probeRep;
                std::unique_ptr<Factorization> probeFact;
                if (agrees) {
                    prof::Scope s("engine.factorize");
                    probeFact = factorize(Ktrue, req.solver, &probeRep);
                }
                if (probeFact && probeFact->ok()) {
                    KFF = std::move(Ktrue);
                    fact = std::move(probeFact);
                    useKrylov = false;
                    diag.probedTangent = true;
                    diag.solver = probeRep.message + " (true tangent assembled from " + std::to_string(us.size()) +
                                  " probes, " + (grouped ? std::to_string(nc) + " column groups)" : std::string("one column each)"));
                }
            }
        }

        std::vector<std::vector<double>> rhsNorm(nD, std::vector<double>(static_cast<size_t>(K) + 1, 0.0));
        auto stencilF = [&](int k, const std::vector<std::vector<double>>& rd) {
            std::vector<double> out(F.size(), 0.0);
            for (size_t q = 0; q < F.size(); ++q) {
                const auto g = static_cast<size_t>(F[q]);
                double s = wAt(k, 0) * r0[g];
                for (size_t b = 0; b < off.size(); ++b) s += wAt(k, off[b]) * rd[b][g];
                out[q] = s;
            }
            return out;
        };
        auto storeSolution = [&](size_t d, int k, const std::vector<double>& x, bool add) {
            auto& tgt = wk[d][static_cast<size_t>(k)];
            if (!add) tgt.assign(ndof, 0.0);
            for (size_t q = 0; q < F.size(); ++q) tgt[static_cast<size_t>(F[q])] += x[q];
        };
        diag.asymmetry.assign(nD, 0.0);
        diag.reprobePasses.assign(nD, 0);
        diag.reprobeConverged.assign(nD, true);
        diag.lowerOrderLeak.assign(nD, 0.0);

        for (int k = 1; k <= K; ++k) {
            if (ctl.cancel) break;
            const auto r = k == 1 ? std::move(firstBranchProbes) : probeAll(k - 1, allDirs, "branch");
            std::vector<std::vector<double>> rhs(nD);
            for (size_t d = 0; d < nD; ++d) {
                rhs[d] = stencilF(k, r[d]);
                rhsNorm[d][static_cast<size_t>(k)] = maxAbs(rhs[d]);
                if (k >= 2 && k == K) {
                    // Lower-order coefficient of the order-K predictor residual must vanish (Noether Thm. 2).
                    const double ref = rhsNorm[d][static_cast<size_t>(k - 1)];
                    const double l = maxAbs(stencilF(k - 1, r[d]));
                    diag.lowerOrderLeak[d] = ref > 0 ? l / ref : l;
                    maxLeak[d] = std::max(maxLeak[d], diag.lowerOrderLeak[d]);
                }
                if (k == 1) {
                    // Order-1 asymmetry: |r(+1) + r(-1) - 2 r0| / |r(+1) - r(-1)| ~ O(h) when smooth.
                    double num = 0, den = 0;
                    for (auto f : F) {
                        const auto g = static_cast<size_t>(f);
                        num = std::max(num, std::abs(r[d][bPlus][g] + r[d][bMinus][g] - 2 * r0[g]));
                        den = std::max(den, std::abs(r[d][bPlus][g] - r[d][bMinus][g]));
                    }
                    // Only meaningful when the first-order change is above noise relative to the force scale
                    // (reactions/loads in r0); otherwise the parameter has no first-order effect here.
                    double scale = 0;
                    for (double x : r0) scale = std::max(scale, std::abs(x));
                    diag.asymmetry[d] = den > 1e-8 * scale && den > 0 ? num / den : 0.0;
                }
            }
            const auto x = solveBlock(rhs);
            for (size_t d = 0; d < nD; ++d) storeSolution(d, k, x[d], false);

            if (k == 1) {
                // Adaptive (and/or forced) re-probe at u* + m s: corrects off-path probes and approximate tangents.
                std::vector<size_t> need;
                for (size_t d = 0; d < nD; ++d)
                    if (req.defectCorrectionPasses > 0 ||
                        (req.asymmetryThreshold > 0 &&
                         diag.asymmetry[d] > std::max(req.asymmetryThreshold, 30.0 * dirs[d].hRel)))
                        need.push_back(d);
                const int passes = std::max(req.defectCorrectionPasses, need.empty() ? 0 : req.maxReprobePasses);
                for (int pass = 0; pass < passes && !need.empty(); ++pass) {
                    const auto rc = probeAll(1, need, "reprobe");
                    std::vector<std::vector<double>> drhs;
                    for (size_t d : need) drhs.push_back(stencilF(1, rc[d]));
                    const auto dx = solveBlock(drhs);
                    std::vector<size_t> still;
                    for (size_t q = 0; q < need.size(); ++q) {
                        const size_t d = need[q];
                        storeSolution(d, 1, dx[q], true);
                        diag.reprobePasses[d]++;
                        double scale = 0;
                        for (double x : uS) scale = std::max(scale, std::abs(x));
                        const double rel = maxAbs(dx[q]) / std::max(maxAbs(wk[d][1]), 1e-14 * std::max(scale, 1e-300));
                        diag.reprobeConverged[d] = rel < 1e-8;
                        if (!diag.reprobeConverged[d] || pass < req.defectCorrectionPasses - 1) still.push_back(d);
                    }
                    need = still;
                }
            }
        }
        if (ctl.cancel) {
            rep.cancelled = true;
            log("Cancelled during increment " + std::to_string(i) + "; it is not stored.");
            break;
        }

        if (gmresRhsThisIncrement > 0) gmresMatvecsPerRhs = double(diag.krylovMatvecs) / gmresRhsThisIncrement;
        // Commit every branch with the full order-K predictor; differentiate committed fields.
        // Commit wave: the replay branch and every perturbed branch commit concurrently (independent histories).
        FrameFields f0;
        std::vector<std::vector<FrameFields>> fd(nD, std::vector<FrameFields>(off.size()));
        diag.commitResidual.assign(nD, 0.0);
        {
            prof::Scope ws("stage.commit_wave");
            // The next increment's order-1 probes (and the replay probe) are at u*_{i+1} on the history these commits
            // create: where the oracle supports it, the commit job evaluates that residual too.
            // On the replay branch, with concurrent jobs, the next tangent-check pair also rides along (sibling jobs that
            // replay this commit): the next increment then needs no separate probe wave. Serially that saves nothing.
            const bool more = i < nFrames;
            std::vector<double> vNext;
            double epsNext = 0;
            auto baseNext = more ? baseProbeStates(i + 1, vNext, epsNext) : std::vector<std::vector<double>>{};
            if (!(req.parallelBranches && req.overlapWaves) && !oracle.batchesWaves() && baseNext.size() > 1) baseNext.resize(1);
            for (auto& row : haveFused) std::fill(row.begin(), row.end(), 0);
            fusedBase.clear();
            // wave[0] = replay branch, then the perturbed branches in (d, b) order
            std::vector<WaveRequest> wave(nD * off.size() + 1);
            wave[0].branch = base.get();
            wave[0].activity = "commit.replay";
            wave[0].commit = true;
            wave[0].commitU = uS;
            wave[0].nextProbes = baseNext;
            for (size_t d = 0; d < nD; ++d)
                for (size_t b = 0; b < off.size(); ++b) {
                    auto& w = wave[1 + d * off.size() + b];
                    w.branch = br[d][b].get();
                    w.activity = "commit.branch";
                    w.commit = true;
                    w.commitU = predictor(d, off[b], K);
                    if (more) w.nextProbes = {uNominal[static_cast<size_t>(i) + 1]};
                }
            if (req.overlapWaves || oracle.batchesWaves()) {
                oracle.runWave(i, wave, req.parallelBranches);
            } else {  // earlier order: replay commit alone, then the branch commits
                std::vector<WaveRequest> first(std::make_move_iterator(wave.begin()), std::make_move_iterator(wave.begin() + 1));
                std::vector<WaveRequest> rest(std::make_move_iterator(wave.begin() + 1), std::make_move_iterator(wave.end()));
                oracle.runWave(i, first, false);
                oracle.runWave(i, rest, req.parallelBranches);
                wave.clear();
                wave.push_back(std::move(first[0]));
                for (auto& w : rest) wave.push_back(std::move(w));
            }
            for (const auto& w : wave) evals += 1 + static_cast<long long>(w.nextResiduals.size());
            f0 = std::move(wave[0].fields);
            fusedBase = std::move(wave[0].nextResiduals);
            for (size_t d = 0; d < nD; ++d)
                for (size_t b = 0; b < off.size(); ++b) {
                    auto& w = wave[1 + d * off.size() + b];
                    fd[d][b] = std::move(w.fields);
                    if (!w.nextResiduals.empty()) fusedProbe[d][b] = std::move(w.nextResiduals[0]), haveFused[d][b] = 1;
                }
        }
        // t-derivatives of every field along every direction: D^k_t F
        std::vector<std::vector<std::vector<std::vector<double>>>> dF(
            nD, std::vector<std::vector<std::vector<double>>>(static_cast<size_t>(K) + 1, std::vector<std::vector<double>>(nFields)));
        for (size_t d = 0; d < nD; ++d) {
            // Predictor consistency on EVERY branch: |r_F(commit_m) - r0| / (|m| * first-order load), smooth ~ C (|m| h)^p.
            double worst = 0;
            double forceScale = 0;  // reactions/loads in r0
            for (double x : r0) forceScale = std::max(forceScale, std::abs(x));
            for (size_t b = 0; b < off.size(); ++b) {
                const auto rc = br[d][b]->lastCommitResidual();
                double num = 0;
                for (auto f : F) num = std::max(num, std::abs(rc[static_cast<size_t>(f)] - r0[static_cast<size_t>(f)]));
                const double den = rhsNorm[d][1] * std::abs(off[b]);
                // A parameter with no first-order load here (e.g. E under displacement control at an elastic increment)
                // has nothing to be inconsistent with: the ratio would divide noise by noise.
                if (den <= 1e-8 * forceScale) continue;
                worst = std::max(worst, num / den);
            }
            diag.commitResidual[d] = worst;
            maxCommit[d] = std::max(maxCommit[d], worst);
            if (!diag.reprobeConverged[d]) engineIssues[d].push_back("re-probe did not converge at increment " + std::to_string(i));
            if (diag.krylovUsed && !diag.krylovConverged)
                engineIssues[d].push_back("GMRES did not reach its tolerance at increment " + std::to_string(i) +
                                          " (residual " + fmt(diag.krylovResidual) + ")");
            for (int k = 1; k <= K; ++k)
                for (size_t fld = 0; fld < nFields; ++fld) {
                    std::vector<const std::vector<double>*> in;
                    std::vector<double> wv;
                    if (wAt(k, 0) != 0.0) {
                        in.push_back(&f0.values[fld]);
                        wv.push_back(wAt(k, 0));
                    }
                    for (size_t b = 0; b < off.size(); ++b) {
                        in.push_back(&fd[d][b].values[fld]);
                        wv.push_back(wAt(k, off[b]));
                    }
                    par::combine(in, wv, dF[d][static_cast<size_t>(k)][fld]);
                }
        }
        // Store: pure d^k/da^k = D^k_t / h^k ; mixed d2/(da db) = (D2_t(a+b) - D2_t(a-b)) / (4 h_a h_b)
        for (size_t d = 0; d < nD; ++d) {
            const auto& dir = dirs[d];
            if (dir.pure >= 0) {
                for (int k = 1; k <= K; ++k) {
                    const double s = 1.0 / std::pow(dir.hPure, k);
                    for (size_t fld = 0; fld < nFields; ++fld) {
                        auto v = dF[d][static_cast<size_t>(k)][fld];
                        for (double& x : v) x *= s;
                        store.set(static_cast<int>(fld), DerivativeSpec::pure(dir.pure, k), i, std::move(v));
                    }
                }
            } else if (dir.sign > 0) {
                const size_t dm2 = d + 1;  // matching a-b direction follows
                const double s = 1.0 / (4.0 * hOf[static_cast<size_t>(dir.mixA)] * hOf[static_cast<size_t>(dir.mixB)]);
                for (size_t fld = 0; fld < nFields; ++fld) {
                    const auto& p = dF[d][2][fld];
                    const auto& m = dF[dm2][2][fld];
                    std::vector<double> v(p.size());
                    for (size_t q = 0; q < v.size(); ++q) v[q] = (p[q] - m[q]) * s;
                    // sign of the (a - b) direction: v_b negative -> D2(a+b) - D2(a-b) = 4 h_a h_b u_ab
                    store.set(static_cast<int>(fld), DerivativeSpec{{dir.mixA, dir.mixB}}, i, std::move(v));
                }
            }
        }
        rep.frames.push_back(diag);
        if (ctl.progress) ctl.progress("Sensitivities", double(i) / nFrames);
        std::ostringstream os;
        os << "Increment " << i << "/" << nFrames << ": tangent check "
           << (diag.tangentCheckError >= 0 ? fmt(diag.tangentCheckError) : "skipped") << ", solve residual "
           << fmt(diag.solveResidual);
        int rp = 0;
        for (int p : diag.reprobePasses) rp += p;
        if (rp) os << ", adaptive re-probe passes " << rp;
        if (diag.probedTangent) os << ", tangent inconsistent -> true tangent assembled from " << 2 * F.size() << " probes";
        if (diag.krylovUsed)
            os << ", tangent inconsistent -> matrix-free GMRES (" << diag.krylovMatvecs << " operator applications, residual "
               << fmt(diag.krylovResidual) << ")";
        log(os.str());
    }

    // Non-smoothness detector. For a smooth branch the commit residual ratio is C (|m| h_rel)^p with p = min(K,2)
    // (Noether Thm. 2; measured O(h) for K = 1, O(h^2) for K = 2) with C ~ 1.6-2 on smooth increments (synthetic
    // models; Gauss S15: 1.8). Flag increments where it exceeds 50 h_rel^p (25x margin) and the 1e-6 round-off
    // floor. Engine failures (re-probe / GMRES non-convergence) are reported separately.
    const int pExp = std::min(K, 2);
    for (size_t d = 0; d < nD; ++d) {
        const double thr = std::max(50.0 * std::pow(dirs[d].hRel, pExp), 1e-6);
        // A non-smooth event is a spike against the run's own level. A strongly nonlinear but smooth model (large
        // rotations: C ~ 170 measured on a shell, validated at 7e-7) raises the ratio at every increment alike, so an
        // increment is suspect only if it is above the absolute threshold AND 10x above the median of this direction.
        std::vector<double> rs;
        for (const auto& fr : rep.frames) rs.push_back(fr.commitResidual.at(d));
        double median = 0;
        if (!rs.empty()) {
            std::nth_element(rs.begin(), rs.begin() + static_cast<std::ptrdiff_t>(rs.size() / 2), rs.end());
            median = rs[rs.size() / 2];
        }
        for (const auto& fr : rep.frames)
            if (fr.commitResidual.at(d) > thr && (rep.frames.size() < 3 || fr.commitResidual.at(d) > 10.0 * median))
                suspicious[d].push_back(fr.frame);
        std::sort(suspicious[d].begin(), suspicious[d].end());
        suspicious[d].erase(std::unique(suspicious[d].begin(), suspicious[d].end()), suspicious[d].end());
    }

    auto makeRecord = [&](size_t d, int k) {
        QualityRecord q;
        q.status = QualityStatus::Unverified;
        q.tangentCheckError = maxTangentErr;
        q.predictorResidualRatio = maxCommit[d];
        q.stepSize = dirs[d].pure >= 0 ? dirs[d].hPure : hOf[static_cast<size_t>(dirs[d].mixA)];
        q.scheme = "history-consistent residual oracle, central stencil m=-" + std::to_string(M) + "..+" +
                   std::to_string(M) + ", order-" + std::to_string(K) + " predictor branches";
        q.notes.push_back("Predictor consistency (commit residual / first-order load), worst over increments and branches: " +
                          fmt(maxCommit[d]) + " (smooth: ~C h^" + std::to_string(std::min(K, 2)) + ", h_rel = " + fmt(dirs[d].hRel) + ").");
        for (const auto& e : engineIssues[d]) q.notes.push_back("ENGINE ISSUE: " + e + " - results at and after it are unreliable.");
        if (!replayBad.empty()) {
            std::string s;
            for (int f : replayBad) s += (s.empty() ? "" : ", ") + std::to_string(f);
            q.status = QualityStatus::Failed;
            q.notes.push_back("ORACLE MISMATCH: replaying the nominal analysis does not reproduce equilibrium at increment(s) " + s +
                              " (free-DOF residual up to " + fmt(worstReplay) + " of the reactions). The model uses behaviour the "
                              "residual oracle does not reproduce; these derivatives are not valid.");
        } else if (worstReplay > 1e-6) {
            q.notes.push_back("Nominal replay residual up to " + fmt(worstReplay) + " of the reactions (nominal equilibrium "
                              "tolerance); derivatives are accurate to roughly this level.");
        }
        if (K >= 2) q.notes.push_back("Max lower-order leak at order " + std::to_string(K) + ": " + fmt(maxLeak[d]));
        {
            int kf = 0, mv = 0;
            double kr = 0;
            for (const auto& fr : rep.frames)
                if (fr.krylovUsed) {
                    ++kf;
                    mv += fr.krylovMatvecs;
                    kr = std::max(kr, fr.krylovResidual);
                }
            if (kf)
                q.notes.push_back("Oracle tangent inconsistent at " + std::to_string(kf) + " increment(s) (tangent check > " +
                                  fmt(req.krylovTrigger) + "); solved matrix-free with GMRES preconditioned by it: " +
                                  std::to_string(mv) + " operator applications, worst relative residual " + fmt(kr) + ".");
        }
        if (!suspicious[d].empty()) {
            std::string s;
            for (int f : suspicious[d]) s += (s.empty() ? "" : ", ") + std::to_string(f);
            q.notes.push_back("Predictor inconsistency at increment(s) " + s + " (above 50 h^p): a non-smooth event "
                              "(yield onset/reversal, contact change) or a probe that left the true perturbed path. "
                              "Values there may be one-sided or wrong, and later increments inherit the history error.");
            q.suspectFrames = suspicious[d];
        }
        if (k >= 3) q.notes.push_back("Order >= 3 is experimental until validated against converged full reruns.");
        return q;
    };
    for (size_t d = 0; d < nD; ++d) {
        if (dirs[d].pure >= 0) {
            for (int k = 1; k <= K; ++k) store.quality[DerivativeSpec::pure(dirs[d].pure, k)] = makeRecord(d, k);
        } else if (dirs[d].sign > 0) {
            auto q = makeRecord(d, 2);
            const auto q2 = makeRecord(d + 1, 2);
            q.predictorResidualRatio = std::max(q.predictorResidualRatio, q2.predictorResidualRatio);
            q.suspectFrames.insert(q.suspectFrames.end(), q2.suspectFrames.begin(), q2.suspectFrames.end());
            q.scheme += "; mixed derivative by polarization over directions a+b and a-b";
            q.notes.push_back("Mixed derivative: experimental until validated against corner reruns.");
            store.quality[DerivativeSpec{{dirs[d].mixA, dirs[d].mixB}}] = q;
        }
    }
    rep.oracleEvaluations = evals.load();
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    log("Sensitivity run finished in " + fmt(rep.seconds) + " s.");
    return rep;
}

}  // namespace abss
