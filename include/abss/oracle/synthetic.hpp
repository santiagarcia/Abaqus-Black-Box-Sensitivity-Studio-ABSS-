// Synthetic (clearly labelled demo) oracle: a small in-house FE model standing in for Abaqus so that the complete
// sensitivity engine and GUI can run, and be validated against analytic answers, without an Abaqus licence.
// It is NOT an Abaqus element library and is never used to analyse a user's INP.
#pragma once

#include "abss/oracle/oracle.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace abss::synthetic {

enum class ElemKind { Truss2, Quad4PlaneStrain, Hex8 };

/// What a model parameter controls.
enum class ParamRole { YoungsModulus, Poisson, YieldStress, Hardening, Area, LoadScale, PrescribedScale, Geometry };

struct ParamDef {
    ParamRole role;
    ParameterInfo info;
};

struct NodalLoad {
    std::int32_t node;
    int dof;          ///< 0-based component
    double value;     ///< magnitude at LoadScale = 1 and amplitude = 1
};

struct Prescribed {
    std::int32_t node;
    int dof;
    double value;     ///< value at PrescribedScale = 1 and amplitude = 1 (0 = fixed support)
    bool scaled = true;  ///< multiplied by the PrescribedScale parameter and amplitude
};

/// Material/geometry values at a given parameter vector.
struct ParamValues {
    double E = 0, nu = 0, sy = 1e300, H = 0, area = 1, loadScale = 1, presScale = 1, geom = 0;
};

struct ModelDef {
    std::string title;
    ParamValues base;  ///< values for roles that are not exposed as parameters
    ElemKind kind = ElemKind::Quad4PlaneStrain;
    int dim = 2;
    std::vector<std::array<double, 3>> X0;          ///< reference coordinates at nominal parameters
    std::vector<std::vector<std::int32_t>> conn;
    std::vector<NodalLoad> loads;
    std::vector<Prescribed> bcs;
    std::vector<double> times;                      ///< accepted increment end times (frame 1..N)
    std::vector<double> amplitude;                  ///< amplitude at each time (same length)
    std::vector<ParamDef> params;
    /// Geometry: coordinates as a function of the geometry parameter value (null = no geometry parameter).
    std::function<std::vector<std::array<double, 3>>(double)> geometry;
    /// Emulate Abaqus *ELEMENT MATRIX OUTPUT under plasticity: tangent() returns the elastic-modulus stiffness
    /// at the current geometry instead of the consistent tangent (used to test the matrix-free Krylov path).
    bool reportElasticTangent = false;
    double newtonTol = 1e-13;                       ///< relative residual tolerance of nominal/perturbed solves
};

/// The synthetic model and its mechanics (residual, consistent tangent, history update).
class Model {
public:
    explicit Model(ModelDef def);
    const ModelDef& def() const { return def_; }
    int dofsPerNode() const { return def_.dim; }
    int ndof() const { return static_cast<int>(def_.X0.size()) * def_.dim; }
    int pointsPerElement() const;
    int stressComponents() const;  ///< 1 truss, 4 plane strain (11,22,33,12), 6 solid
    ParamValues values(const std::vector<double>& delta) const;
    std::vector<std::array<double, 3>> coordinates(const ParamValues& pv) const;

    struct State {  ///< per material point
        std::vector<double> epsP;   ///< plastic strain (stressComponents each, tensor Voigt with engineering shear /2 stored as tensor)
        std::vector<double> alpha;  ///< equivalent plastic strain
    };
    State initialState() const;

    struct Eval {
        std::vector<double> fint;           ///< internal force (ndof)
        std::vector<Triplet> K;             ///< consistent tangent dFint/du (if requested)
        State state;                        ///< updated state
        std::vector<double> stress, strain; ///< per material point
    };
    /// Evaluate internal forces at total displacement u from committed state `prev` (backward-Euler return map).
    Eval evaluate(const std::vector<double>& u, const State& prev, const ParamValues& pv, bool tangent) const;
    std::vector<double> externalForce(int frame, const ParamValues& pv) const;
    /// Prescribed DOF indices and their values at a frame.
    std::vector<std::pair<std::int32_t, double>> prescribedValues(int frame, const ParamValues& pv) const;

private:
    ModelDef def_;
};

/// Oracle backed by the synthetic model (exact residuals, exact consistent tangent).
class SyntheticOracle final : public Oracle {
public:
    explicit SyntheticOracle(ModelDef def);
    OracleCapabilities capabilities() const override;
    std::vector<std::vector<double>> runNominal(ResultStore& store, const ProgressFn& progress) override;
    const DofMap& dofMap() const override { return dofs_; }
    CsrMatrix tangent(int frame) override;
    std::unique_ptr<HistoryBranch> openBranch(const std::vector<double>& delta) override;
    std::vector<std::vector<double>> solvePerturbed(const std::vector<double>& delta, std::vector<FrameFields>* fields) override;

    const Model& model() const { return model_; }
    /// Number of oracle evaluations performed (residual probes + commits), for cost reporting.
    long long evaluations() const { return evaluations_; }
    void countEvaluation(int n = 1) { evaluations_ += n; }

    /// Build output fields (U, RF, S, E, PEEQ, S_Mises) for a state.
    FrameFields makeFields(const std::vector<double>& u, const Model::Eval& ev, const std::vector<double>& fext) const;

private:
    Model model_;
    DofMap dofs_;
    std::vector<std::vector<double>> uNominal_;
    std::vector<Model::State> stateNominal_;  ///< committed state after each frame (index 0 = initial)
    long long evaluations_ = 0;
};

// ---- Bundled models -------------------------------------------------------------------------------------------
/// Two collinear truss bars, node 1 fixed, force F at node 2, prescribed displacement g at node 3 (analytic answers).
ModelDef twoBarTruss(bool plastic);
/// Quarter plate with a circular hole, plane strain, J2 plasticity, load-unload-reload history. Geometry: hole radius.
ModelDef plateWithHole(int nRadial = 8, int nAngular = 10);
/// 3D cantilever block (C3D8-like hexes), elastic-plastic, tip load.
ModelDef cantileverBlock(int nx = 10, int ny = 3, int nz = 3);
/// Single plane-strain element pulled by a prescribed displacement (uniform state; analytic elastic answers).
ModelDef singleQuad(bool plastic);

}  // namespace abss::synthetic

