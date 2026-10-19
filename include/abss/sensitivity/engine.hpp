// ABSS sensitivity engine: history-consistent, order-K pure parameter sensitivities over every accepted increment.
//
// Algorithm (docs/SCIENCE.md §3-§5): for each parameter a_j and stencil offsets m in {-M..M}, a history branch at
// a_j + m h is advanced in lock-step with the nominal replay branch (m = 0). At frame i and order k = 1..K:
//   predictor_m = u_i* + sum_{l<k} (m h)^l / l! u_i^(l)                 (free DOFs; BCs imposed by the branch at a+mh)
//   K_FF u_i^(k) = D_h^k [ r_F(predictor_m, branch-m history, a + m h) ]  (central FD stencil over m, Form B)
// then every branch commits u_i* + sum_{l<=K} (m h)^l / l! u_i^(l) so its history is correct to O(h^(K+1)).
// Output fields (S, PEEQ, Mises, RF ...) are differentiated with the same stencil over the committed branch states.
#pragma once

#include "abss/oracle/oracle.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace abss {

/// Central-difference weights w_m (m = -M..M) such that f^(k)(0) ~ sum_m w_m f(m h) / h^k. Fornberg's algorithm.
std::vector<double> stencilWeights(const std::vector<int>& offsets, int k);
/// Default half width M for maximum order K (O(h^2) central accuracy for every order <= K).
int defaultHalfWidth(int maxOrder);

/// Default relative step |h/a| for a run whose highest order is K (Gauss review §8: ~eps^(1/(k+2)) tempered by
/// the plasticity "straddle" evidence in docs/VALIDATION_RESULTS.md): 1e-5, 1e-4, 1e-3 for K = 1, 2, >=3.
double defaultRelativeStep(int maxOrder);

struct SensitivityRequest {
    std::vector<int> parameters;        ///< indices into ResultStore::parameters
    int maxOrder = 1;                   ///< K >= 1
    std::vector<double> absoluteSteps;  ///< per requested parameter; empty = defaultRelativeStep(K) * |nominal|
    int halfWidth = 0;                  ///< M; 0 = defaultHalfWidth(maxOrder)
    /// Mixed second derivatives d2/(da db) by polarization over directions a+b and a-b (pairs of parameter indices
    /// that must also appear in `parameters`). Experimental until validated.
    std::vector<std::pair<int, int>> mixedPairs;
    SolverKind solver = SolverKind::Auto;
    bool tangentCheck = true;           ///< K v vs. central FD of oracle residual (2 probes per frame)
    int defectCorrectionPasses = 0;     ///< forced first-order re-probe passes (approximate tangents)
    /// Adaptive re-probe (Gauss S15): when the order-1 probe asymmetry |r+ + r- - 2 r0| / |r+ - r-| exceeds
    /// max(asymmetryThreshold, 30 h_rel) (smooth problems give ~1.8 h_rel), re-probe at u* + m h s until the update
    /// is below 1e-8 relative (at most `maxReprobePasses`). 0 disables.
    double asymmetryThreshold = 0.01;
    int maxReprobePasses = 8;
    bool parallelBranches = false;      ///< evaluate branches concurrently (oracle must allow it)
    /// Matrix-free Krylov refinement when the oracle tangent is not the consistent tangent (Abaqus returns the
    /// elastic-modulus stiffness under built-in plasticity: docs/council/scout_abaqus2024.md). Operator K_true v is
    /// the central FD of probes on the nominal replay branch; the oracle tangent's factorization preconditions GMRES.
    /// Triggered when the tangent check exceeds `krylovTrigger` (requires tangentCheck) or always when forced.
    double krylovTrigger = 1e-5;
    bool forceKrylov = false;
    double krylovTolerance = 1e-8;      ///< relative residual of GMRES (FD-operator noise is ~1e-10 relative)
    int krylovMaxIterations = 60;
};

struct RunControl {
    std::atomic<bool> cancel{false};
    std::function<void(const std::string& stage, double fraction)> progress;
    std::function<void(const std::string& line)> log;
};

struct FrameDiagnostics {
    int frame = 0;
    double tangentCheckError = -1;       ///< ||K v - Dr v|| / ||K v||
    double solveResidual = 0;            ///< max relative linear-solve residual
    std::vector<double> commitResidual;  ///< per parameter: max_F |r(commit, m=+1)| / max_F |D^1 r| h  (~O(h^K))
    std::vector<double> lowerOrderLeak;  ///< per parameter: |D^{K-1} r(pred_K)| relative (should be ~0, K>=2)
    std::vector<double> asymmetry;       ///< per direction: order-1 probe asymmetry (smooth ~ O(h))
    std::vector<int> reprobePasses;      ///< per direction: adaptive re-probe passes used
    std::vector<bool> reprobeConverged;  ///< per direction
    bool krylovUsed = false;
    int krylovMatvecs = 0;               ///< operator applications (2 oracle probes each)
    double krylovResidual = 0;           ///< worst final relative residual over right-hand sides
    bool krylovConverged = true;         ///< false = engine failure at this increment (reported, never hidden)
    std::string solver;
};

struct SensitivityReport {
    std::vector<FrameDiagnostics> frames;
    long long oracleEvaluations = 0;
    double seconds = 0;
    std::string computeBackend;
    bool cancelled = false;
};

/// Oracle evaluations a run will need (probes + commits), not including validation reruns.
long long estimateOracleEvaluations(int frames, int params, int maxOrder, int halfWidth, bool tangentCheck);

/// Run the engine. `uNominal` from Oracle::runNominal; results are written into `store` with DerivativeSpec::pure.
SensitivityReport runSensitivities(Oracle& oracle, const std::vector<std::vector<double>>& uNominal,
                                   const SensitivityRequest& req, ResultStore& store, RunControl& ctl);

// ---- Validation -------------------------------------------------------------------------------------------------
struct ValidationOptions {
    std::vector<double> tolerance = {1e-3, 1e-2, 5e-2, 1e-1};  ///< accepted max relative error per order 1..4
    double stepFactor = 1.0;  ///< validation reruns use stepFactor * engine step
    std::vector<std::string> fields;  ///< empty = all fields
};

/// Compare engine derivatives (every frame, every field) to central FD of fully re-equilibrated reruns at a +- m h.
/// Updates store.quality. Returns a human-readable summary.
std::string validateAgainstReruns(Oracle& oracle, const SensitivityRequest& req, ResultStore& store,
                                  const ValidationOptions& opt, RunControl& ctl);

}  // namespace abss
