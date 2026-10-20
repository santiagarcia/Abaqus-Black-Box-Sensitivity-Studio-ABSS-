// ABSS oracle interface: Abaqus (or a synthetic stand-in) as an opaque residual/tangent operator.
//
// Sign convention (docs/SCIENCE.md §1):  r(u) = F_ext - F_int(u),   K = -dr/du  (positive stiffness).
// All vectors are indexed by DofMap (independent DOFs only).
#pragma once

#include "abss/core/results.hpp"
#include "abss/linalg/sparse.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace abss {

struct DofId {
    std::int32_t node = 0;  ///< Mesh node index
    std::int8_t dof = 1;    ///< Abaqus DOF 1..6
};

/// Independent DOFs of the model and which of them are prescribed by the *original* model's BCs.
struct DofMap {
    std::vector<DofId> dofs;
    std::vector<std::uint8_t> prescribed;  ///< 1 = originally prescribed (Dirichlet) DOF
    std::int32_t size() const { return static_cast<std::int32_t>(dofs.size()); }
    std::vector<std::int32_t> freeIndices() const;
    std::vector<std::int32_t> prescribedIndices() const;
};

/// Per-frame field values produced by a converged or committed state (all fields of ResultStore::fields).
struct FrameFields {
    std::vector<std::vector<double>> values;  ///< [field][location*comp]
};

/// Capability matrix: what this backend can do and how trustworthy each part is.
struct OracleCapabilities {
    std::string backendName;
    bool synthetic = false;
    bool exactTangent = false;        ///< tangent is the exact consistent tangent of r
    bool supportsHistory = true;      ///< path-dependent materials
    bool canRunInParallel = false;
    std::vector<std::string> notes;
};

/// A perturbed-history branch at parameter values a + delta. The branch owns a committed history
/// (u_0..u_{i-1}, internal state q). Residual probes never modify that history; commit() does.
class HistoryBranch {
public:
    virtual ~HistoryBranch() = default;
    /// Residuals r_i(u, committed history through i-1, a+delta) for a batch of candidate current
    /// displacements. Each u is a full independent-DOF vector; originally prescribed DOFs in u are
    /// IGNORED and replaced by the branch's own (perturbed) prescribed values g_i(a+delta).
    /// Returns one full-length residual vector per probe; entries on prescribed DOFs are unused.
    virtual std::vector<std::vector<double>> residuals(int frame, const std::vector<std::vector<double>>& u) = 0;
    /// Accept increment `frame` with current displacement u (free DOFs as given, prescribed DOFs from
    /// the perturbed BCs). Returns the field outputs of the committed state and the actual displacement
    /// (so callers can read the perturbed prescribed values g_i(a+delta)).
    virtual FrameFields commit(int frame, const std::vector<double>& u, std::vector<double>* committedU) = 0;
    /// Residual of the most recent commit (predictor consistency diagnostic).
    virtual std::vector<double> lastCommitResidual() const = 0;
};

using ProgressFn = std::function<void(const std::string& stage, double fraction)>;

class Oracle {
public:
    virtual ~Oracle() = default;
    virtual OracleCapabilities capabilities() const = 0;

    /// Run the nominal analysis; fills store.mesh, frames (index 0 = initial state), fields
    /// (nominal series) and parameters; returns converged displacements u_i* for every frame.
    virtual std::vector<std::vector<double>> runNominal(ResultStore& store, const ProgressFn& progress) = 0;

    virtual const DofMap& dofMap() const = 0;
    /// Unknown (free) DOFs at frame i. Differs from dofMap().freeIndices() only when the constrained set changes
    /// between steps (a boundary condition added or released): a DOF is free in the frames where no BC holds it.
    virtual std::vector<std::int32_t> freeIndicesAt(int frame) const {
        (void)frame;
        return dofMap().freeIndices();
    }

    /// Tangent K_i = -dr_i/du at the converged nominal state of frame i (full independent-DOF square matrix).
    virtual CsrMatrix tangent(int frame) = 0;

    /// Open a branch at parameter offsets delta (size = parameter count) from t = 0.
    virtual std::unique_ptr<HistoryBranch> openBranch(const std::vector<double>& delta) = 0;

    /// Reference: fully re-equilibrated analysis at a + delta (used only for validation).
    /// Returns displacements per frame and fills `fields` per frame. Empty on failure (e.g. cutbacks).
    virtual std::vector<std::vector<double>> solvePerturbed(const std::vector<double>& delta,
                                                            std::vector<FrameFields>* fields) = 0;
};

}  // namespace abss
