#include "abss/oracle/synthetic.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace abss::synthetic {

namespace {

const char* elementTypeName(ElemKind k) {
    switch (k) {
        case ElemKind::Truss2: return "T2D2";
        case ElemKind::Quad4PlaneStrain: return "CPE4";
        case ElemKind::Hex8: return "C3D8";
    }
    return "?";
}

ElementTypeInfo elementInfo(ElemKind k) {
    switch (k) {
        case ElemKind::Truss2: return {ElementShape::Line2, ElementFamily::Truss, 1, 2};
        case ElemKind::Quad4PlaneStrain: return {ElementShape::Quad4, ElementFamily::Continuum2D, 2, 4};
        case ElemKind::Hex8: return {ElementShape::Hex8, ElementFamily::Continuum3D, 3, 8};
    }
    return {};
}

double mises(const double* s, int nsc) {
    if (nsc == 1) return std::abs(s[0]);
    const double s11 = s[0], s22 = s[1], s33 = s[2], s12 = s[3];
    const double s13 = nsc == 6 ? s[4] : 0.0, s23 = nsc == 6 ? s[5] : 0.0;
    return std::sqrt(0.5 * ((s11 - s22) * (s11 - s22) + (s22 - s33) * (s22 - s33) + (s33 - s11) * (s33 - s11)) +
                     3.0 * (s12 * s12 + s13 * s13 + s23 * s23));
}

ParameterInfo makeParam(const std::string& id, const std::string& name, const std::string& desc, ParamCategory cat,
                        double nominal, bool positive) {
    ParameterInfo p;
    p.id = id;
    p.name = name;
    p.description = desc;
    p.category = cat;
    p.confidence = Confidence::KnownSemantic;
    p.nominal = nominal;
    p.mustBePositive = positive;
    if (positive) p.lowerBound = 0.0;
    p.relativeStep = 1e-4;  // plasticity: smaller steps avoid straddling yield onsets (see VALIDATION_RESULTS)
    p.association = "synthetic demo model";
    p.steps = {"Step-1"};
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
class SyntheticBranch final : public HistoryBranch {
public:
    SyntheticBranch(SyntheticOracle& o, std::vector<double> delta)
        : o_(o), pv_(o.model().values(delta)), state_(o.model().initialState()),
          u_(static_cast<size_t>(o.model().ndof()), 0.0) {}

    std::vector<std::vector<double>> residuals(int frame, const std::vector<std::vector<double>>& us) override {
        requireNext(frame);
        std::vector<std::vector<double>> out;
        const auto fext = o_.model().externalForce(frame, pv_);
        for (const auto& u0 : us) {
            auto u = withBcs(frame, u0);
            const auto ev = o_.model().evaluate(u, state_, pv_, false);
            std::vector<double> r(fext.size());
            for (size_t i = 0; i < r.size(); ++i) r[i] = fext[i] - ev.fint[i];
            out.push_back(std::move(r));
            o_.countEvaluation();
        }
        return out;
    }

    FrameFields commit(int frame, const std::vector<double>& u0, std::vector<double>* committedU) override {
        requireNext(frame);
        auto u = withBcs(frame, u0);
        const auto ev = o_.model().evaluate(u, state_, pv_, false);
        const auto fext = o_.model().externalForce(frame, pv_);
        lastResidual_.assign(fext.size(), 0.0);
        for (size_t i = 0; i < fext.size(); ++i) lastResidual_[i] = fext[i] - ev.fint[i];
        state_ = ev.state;
        u_ = u;
        committed_ = frame;
        o_.countEvaluation();
        if (committedU) *committedU = u;
        return o_.makeFields(u, ev, fext);
    }

    std::vector<double> lastCommitResidual() const override { return lastResidual_; }

private:
    void requireNext(int frame) const {
        if (frame != committed_ + 1)
            throw std::logic_error("history branch: frame " + std::to_string(frame) + " requested but history is at " +
                                   std::to_string(committed_));
    }
    std::vector<double> withBcs(int frame, std::vector<double> u) const {
        for (const auto& [d, v] : o_.model().prescribedValues(frame, pv_)) u[static_cast<size_t>(d)] = v;
        return u;
    }

    SyntheticOracle& o_;
    ParamValues pv_;
    Model::State state_;
    std::vector<double> u_;
    std::vector<double> lastResidual_;
    int committed_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
SyntheticOracle::SyntheticOracle(ModelDef def) : model_(std::move(def)) {
    const int dim = model_.def().dim;
    const auto nn = static_cast<std::int32_t>(model_.def().X0.size());
    for (std::int32_t n = 0; n < nn; ++n)
        for (int d = 0; d < dim; ++d) dofs_.dofs.push_back({n, static_cast<std::int8_t>(d + 1)});
    dofs_.prescribed.assign(dofs_.dofs.size(), 0);
    for (const auto& b : model_.def().bcs) dofs_.prescribed[static_cast<size_t>(b.node * dim + b.dof)] = 1;
}

OracleCapabilities SyntheticOracle::capabilities() const {
    OracleCapabilities c;
    c.backendName = "Synthetic demo model (in-house small-strain FE, not Abaqus)";
    c.synthetic = true;
    c.exactTangent = !model_.def().reportElasticTangent;
    c.supportsHistory = true;
    c.canRunInParallel = true;
    c.notes = {"Synthetic data: generated by a built-in J2-plasticity test model, not by Abaqus.",
               "Exact consistent tangent; residuals evaluated in double precision."};
    return c;
}

FrameFields SyntheticOracle::makeFields(const std::vector<double>& u, const Model::Eval& ev,
                                        const std::vector<double>& fext) const {
    const int dim = model_.def().dim;
    const size_t nn = model_.def().X0.size();
    const int nsc = model_.stressComponents();
    FrameFields f;
    std::vector<double> U(nn * static_cast<size_t>(dim)), RF(U.size());
    for (size_t i = 0; i < U.size(); ++i) {
        U[i] = u[i];
        RF[i] = ev.fint[i] - fext[i];  // = -r : Abaqus RF semantics (reaction at every held DOF)
    }
    const size_t npts = ev.state.alpha.size();
    std::vector<double> mis(npts);
    for (size_t p = 0; p < npts; ++p) mis[p] = mises(&ev.stress[p * static_cast<size_t>(nsc)], nsc);
    f.values = {U, RF, ev.stress, ev.strain, ev.state.alpha, mis};
    return f;
}

std::vector<std::vector<double>> SyntheticOracle::runNominal(ResultStore& store, const ProgressFn& progress) {
    const auto& d = model_.def();
    store.title = d.title;
    store.backend = "synthetic-demo";
    store.synthetic = true;
    store.mesh = Mesh{};
    store.mesh.name = d.title;
    store.mesh.spatialDim = d.dim;
    const auto pv0 = model_.values({});
    const auto X = model_.coordinates(pv0);
    for (size_t n = 0; n < X.size(); ++n) store.mesh.addNode({-1, static_cast<std::int64_t>(n + 1)}, X[n]);
    for (size_t e = 0; e < d.conn.size(); ++e) {
        Element el;
        el.label = static_cast<std::int64_t>(e + 1);
        el.type = elementTypeName(d.kind);
        el.info = elementInfo(d.kind);
        el.conn = d.conn[e];
        el.integrationPoints = model_.pointsPerElement();
        store.mesh.elements.push_back(std::move(el));
    }
    store.points = PointLayout::uniform(static_cast<std::int32_t>(d.conn.size()), model_.pointsPerElement());

    std::vector<std::string> uc, rc;
    for (int i = 1; i <= d.dim; ++i) {
        uc.push_back("U" + std::to_string(i));
        rc.push_back("RF" + std::to_string(i));
    }
    std::vector<std::string> sc, ec;
    const int nsc = model_.stressComponents();
    static const char* names[] = {"11", "22", "33", "12", "13", "23"};
    for (int k = 0; k < nsc; ++k) {
        sc.push_back(std::string("S") + names[k]);
        ec.push_back(std::string("E") + names[k]);
    }
    store.fields = {
        {"U", FieldLocation::Node, FieldKind::Vector, uc, "Displacement", ""},
        {"RF", FieldLocation::Node, FieldKind::Vector, rc, "Reaction force (residual at held DOFs)", ""},
        {"S", FieldLocation::IntegrationPoint, FieldKind::SymTensor, sc, "Cauchy stress", ""},
        {"E", FieldLocation::IntegrationPoint, FieldKind::SymTensor, ec, "Total strain (engineering shear)", ""},
        {"PEEQ", FieldLocation::IntegrationPoint, FieldKind::Scalar, {"PEEQ"}, "Equivalent plastic strain", ""},
        {"S_Mises", FieldLocation::IntegrationPoint, FieldKind::Scalar, {"Mises"}, "von Mises stress (invariant)", ""},
    };
    store.parameters.clear();
    for (const auto& p : d.params) store.parameters.push_back(p.info);
    store.frames.clear();
    store.frames.push_back({1, "Step-1", 0, 0.0, 0.0});
    for (size_t i = 0; i < d.times.size(); ++i)
        store.frames.push_back({1, "Step-1", static_cast<int>(i + 1), d.times[i], d.times[i]});
    store.clearSeries();

    std::vector<FrameFields> fields;
    uNominal_ = solvePerturbed({}, &fields);
    if (uNominal_.empty()) throw std::runtime_error("Synthetic nominal analysis did not converge");
    // Record committed states along the nominal path (needed for tangents).
    stateNominal_.assign(1, model_.initialState());
    for (size_t i = 1; i < uNominal_.size(); ++i)
        stateNominal_.push_back(model_.evaluate(uNominal_[i], stateNominal_.back(), pv0, false).state);
    for (size_t fr = 0; fr < fields.size(); ++fr) {
        for (size_t k = 0; k < store.fields.size(); ++k) store.set(static_cast<int>(k), DerivativeSpec::nominal(), static_cast<int>(fr), fields[fr].values[k]);
        if (progress) progress("Nominal analysis", double(fr + 1) / double(fields.size()));
    }
    store.log.push_back("Nominal synthetic analysis: " + std::to_string(fields.size() - 1) + " increments converged.");
    return uNominal_;
}

CsrMatrix SyntheticOracle::tangent(int frame) {
    if (frame < 1 || static_cast<size_t>(frame) >= uNominal_.size()) throw std::out_of_range("tangent: frame");
    auto pv = model_.values({});
    if (model_.def().reportElasticTangent) pv.sy = 1e300;  // elastic predictor stiffness, like Abaqus EMO under plasticity
    const auto ev = model_.evaluate(uNominal_[static_cast<size_t>(frame)], stateNominal_[static_cast<size_t>(frame - 1)], pv, true);
    return CsrMatrix::fromTriplets(model_.ndof(), model_.ndof(), ev.K);
}

std::unique_ptr<HistoryBranch> SyntheticOracle::openBranch(const std::vector<double>& delta) {
    return std::make_unique<SyntheticBranch>(*this, delta);
}

std::vector<std::vector<double>> SyntheticOracle::solvePerturbed(const std::vector<double>& delta,
                                                                 std::vector<FrameFields>* fields) {
    const auto pv = model_.values(delta);
    const int n = model_.ndof();
    const auto freeIdx = dofs_.freeIndices();
    std::vector<std::vector<double>> us;
    std::vector<double> u(static_cast<size_t>(n), 0.0);
    auto state = model_.initialState();
    {
        const auto ev = model_.evaluate(u, state, pv, false);
        us.push_back(u);
        if (fields) {
            fields->clear();
            fields->push_back(makeFields(u, ev, model_.externalForce(0, pv)));
        }
    }
    const int nframes = static_cast<int>(model_.def().times.size());
    for (int fr = 1; fr <= nframes; ++fr) {
        for (const auto& [dd, v] : model_.prescribedValues(fr, pv)) u[static_cast<size_t>(dd)] = v;
        const auto fext = model_.externalForce(fr, pv);
        bool converged = false;
        Model::Eval ev;
        // Newton with consistent tangent + backtracking line search on ||r_F||_2 (robust at unloading reversals).
        auto residualF = [&](const Model::Eval& e, std::vector<double>& rF) {
            double r2 = 0;
            rF.resize(freeIdx.size());
            for (size_t i = 0; i < freeIdx.size(); ++i) {
                const auto g = static_cast<size_t>(freeIdx[i]);
                rF[i] = fext[g] - e.fint[g];
                r2 += rF[i] * rF[i];
            }
            return std::sqrt(r2);
        };
        std::vector<double> rF;
        double prevRn = 1e300;
        for (int it = 0; it < 60; ++it) {
            ev = model_.evaluate(u, state, pv, true);
            const double r2 = residualF(ev, rF);
            double rn = 0, scale = 1e-30;
            for (double v : rF) rn = std::max(rn, std::abs(v));
            for (size_t i = 0; i < fext.size(); ++i) scale = std::max({scale, std::abs(fext[i]), std::abs(ev.fint[i])});
            // Converged at the requested tolerance, or stagnated at the round-off floor (< 1e-10 of the force scale).
            if (rn <= model_.def().newtonTol * scale || (rn <= 1e-10 * scale && rn >= 0.5 * prevRn)) {
                converged = true;
                break;
            }
            prevRn = rn;
            const auto K = CsrMatrix::fromTriplets(n, n, ev.K).submatrix(freeIdx, freeIdx);
            const auto du = solveMany(K, {rF}, SolverKind::Auto, nullptr);
            double step = 1.0;
            std::vector<double> trial = u, rT;
            for (int ls = 0; ls < 12; ++ls) {
                trial = u;
                for (size_t i = 0; i < freeIdx.size(); ++i) trial[static_cast<size_t>(freeIdx[i])] += step * du[0][i];
                if (residualF(model_.evaluate(trial, state, pv, false), rT) < r2 || ls == 11) break;
                step *= 0.5;
            }
            u = trial;
        }
        if (!converged) return {};
        ev = model_.evaluate(u, state, pv, false);
        state = ev.state;
        us.push_back(u);
        if (fields) fields->push_back(makeFields(u, ev, fext));
    }
    return us;
}

// ---- Bundled models ---------------------------------------------------------------------------------------------

ModelDef twoBarTruss(bool plastic) {
    ModelDef d;
    d.title = plastic ? "Two-bar truss (elastic-plastic)" : "Two-bar truss (elastic)";
    d.kind = ElemKind::Truss2;
    d.dim = 2;
    const double L = 10.0;
    d.X0 = {{0, 0, 0}, {L, 0, 0}, {2 * L, 0, 0}};
    d.conn = {{0, 1}, {1, 2}};
    d.loads = {{1, 0, 1.0}};
    d.bcs = {{0, 0, 0.0, false}, {0, 1, 0.0, false}, {1, 1, 0.0, false}, {2, 1, 0.0, false}, {2, 0, 1.0, true}};
    if (plastic) {
        d.times = {0.2, 0.4, 0.6, 0.8, 1.0, 1.2};
        d.amplitude = {1.0, 2.0, 3.0, 4.0, 2.5, 1.0};
    } else {
        d.times = {0.25, 0.5, 0.75, 1.0};
        d.amplitude = {0.25, 0.5, 0.75, 1.0};
    }
    d.base.nu = 0.3;
    d.params.push_back({ParamRole::YoungsModulus, makeParam("syn:E", "E", "Young's modulus of both bars", ParamCategory::Material, 200000.0, true)});
    d.params.push_back({ParamRole::Area, makeParam("syn:A", "A", "Cross-section area of both bars", ParamCategory::Section, 2.0, true)});
    d.params.push_back({ParamRole::LoadScale, makeParam("syn:F", "F", "Force applied at the middle node (direction 1)", ParamCategory::Load, 1000.0, false)});
    d.params.push_back({ParamRole::PrescribedScale, makeParam("syn:g", "g", "Prescribed displacement of the right end (direction 1)", ParamCategory::Boundary, 0.01, false)});
    if (plastic) {
        d.params.push_back({ParamRole::YieldStress, makeParam("syn:sy", "sigma_y", "Initial yield stress", ParamCategory::Material, 250.0, true)});
        d.params.push_back({ParamRole::Hardening, makeParam("syn:H", "H", "Linear isotropic hardening modulus", ParamCategory::Material, 20000.0, true)});
        d.params[2].info.nominal = 400.0;   // F
        d.params[3].info.nominal = 0.004;   // g
    }
    return d;
}

ModelDef singleQuad(bool plastic) {
    ModelDef d;
    d.title = plastic ? "Single CPE4 element (elastic-plastic)" : "Single CPE4 element (elastic)";
    d.kind = ElemKind::Quad4PlaneStrain;
    d.dim = 2;
    d.X0 = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    d.conn = {{0, 1, 2, 3}};
    d.bcs = {{0, 0, 0.0, false}, {0, 1, 0.0, false}, {1, 1, 0.0, false}, {3, 0, 0.0, false},
             {2, 1, 1.0, true},  {3, 1, 1.0, true}};
    if (plastic) {
        d.times = {0.2, 0.4, 0.6, 0.8, 1.0};
        d.amplitude = {0.5, 1.0, 1.5, 0.8, 0.0};
    } else {
        d.times = {0.5, 1.0};
        d.amplitude = {0.5, 1.0};
    }
    d.params.push_back({ParamRole::YoungsModulus, makeParam("syn:E", "E", "Young's modulus", ParamCategory::Material, 200000.0, true)});
    d.params.push_back({ParamRole::Poisson, makeParam("syn:nu", "nu", "Poisson's ratio", ParamCategory::Material, 0.3, false)});
    d.params.back().info.lowerBound = -1.0;
    d.params.back().info.upperBound = 0.5;
    d.params.push_back({ParamRole::PrescribedScale, makeParam("syn:g", "g", "Prescribed vertical displacement of the top edge", ParamCategory::Boundary, 0.002, false)});
    if (plastic) {
        d.params.push_back({ParamRole::YieldStress, makeParam("syn:sy", "sigma_y", "Initial yield stress", ParamCategory::Material, 250.0, true)});
        d.params.push_back({ParamRole::Hardening, makeParam("syn:H", "H", "Linear isotropic hardening modulus", ParamCategory::Material, 10000.0, true)});
    }
    return d;
}

ModelDef plateWithHole(int nR, int nA) {
    ModelDef d;
    d.title = "Plate with a hole (synthetic demo, plane strain, J2 plasticity)";
    d.kind = ElemKind::Quad4PlaneStrain;
    d.dim = 2;
    const double W = 10.0;
    auto mesh = [nR, nA, W](double R) {
        std::vector<std::array<double, 3>> X;
        for (int j = 0; j <= nA; ++j) {
            const double th = 0.5 * std::numbers::pi * double(j) / nA;
            const double ox = th <= std::numbers::pi / 4 ? W : W / std::tan(th);
            const double oy = th <= std::numbers::pi / 4 ? W * std::tan(th) : W;
            const double ix = R * std::cos(th), iy = R * std::sin(th);
            for (int i = 0; i <= nR; ++i) {
                const double s = std::pow(double(i) / nR, 1.4);
                X.push_back({ix + s * (ox - ix), iy + s * (oy - iy), 0.0});
            }
        }
        return X;
    };
    const double R0 = 2.0;
    d.X0 = mesh(R0);
    d.geometry = mesh;
    auto id = [nR](int i, int j) { return j * (nR + 1) + i; };
    for (int j = 0; j < nA; ++j)
        for (int i = 0; i < nR; ++i) d.conn.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
    for (int i = 0; i <= nR; ++i) {
        d.bcs.push_back({id(i, 0), 1, 0.0, false});   // y = 0 symmetry: U2 = 0
        d.bcs.push_back({id(i, nA), 0, 0.0, false});  // x = 0 symmetry: U1 = 0
    }
    // Top edge (y = W): prescribed U2; right edge (x = W): traction as consistent nodal forces.
    for (int j = 0; j <= nA; ++j) {
        const double th = 0.5 * std::numbers::pi * double(j) / nA;
        if (th >= std::numbers::pi / 4 - 1e-12) d.bcs.push_back({id(nR, j), 1, 1.0, true});
    }
    const auto& X = d.X0;
    for (int j = 0; j < nA; ++j) {
        const auto a = id(nR, j), b = id(nR, j + 1);
        if (std::abs(X[static_cast<size_t>(a)][0] - W) < 1e-9 && std::abs(X[static_cast<size_t>(b)][0] - W) < 1e-9) {
            const double len = std::abs(X[static_cast<size_t>(b)][1] - X[static_cast<size_t>(a)][1]);
            d.loads.push_back({a, 0, 0.5 * len});
            d.loads.push_back({b, 0, 0.5 * len});
        }
    }
    d.times = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
    // Reload deliberately avoids the previous peak (1.0): re-yield exactly at a past maximum is a kink.
    d.amplitude = {0.25, 0.5, 0.75, 1.0, 0.7, 0.4, 0.2, 0.6, 0.95, 1.2};
    d.params.push_back({ParamRole::YoungsModulus, makeParam("syn:E", "E", "Young's modulus of the plate", ParamCategory::Material, 210000.0, true)});
    d.params.push_back({ParamRole::Poisson, makeParam("syn:nu", "nu", "Poisson's ratio of the plate", ParamCategory::Material, 0.3, false)});
    d.params.back().info.lowerBound = -1.0;
    d.params.back().info.upperBound = 0.5;
    d.params.push_back({ParamRole::YieldStress, makeParam("syn:sy", "sigma_y", "Initial yield stress", ParamCategory::Material, 250.0, true)});
    d.params.push_back({ParamRole::Hardening, makeParam("syn:H", "H", "Linear isotropic hardening modulus", ParamCategory::Material, 2000.0, true)});
    d.params.push_back({ParamRole::PrescribedScale, makeParam("syn:g", "g_top", "Prescribed vertical displacement of the top edge", ParamCategory::Boundary, 0.02, false)});
    d.params.push_back({ParamRole::LoadScale, makeParam("syn:p", "p_right", "Traction (force per length) on the right edge", ParamCategory::Load, 60.0, false)});
    d.params.push_back({ParamRole::Geometry, makeParam("syn:R", "R_hole", "Hole radius (mesh nodes move, topology fixed)", ParamCategory::Geometry, R0, true)});
    d.params.back().info.upperBound = 9.0;
    d.params.back().info.mode = EditMode::Offset;
    return d;
}

ModelDef cantileverBlock(int nx, int ny, int nz) {
    ModelDef d;
    d.title = "Cantilever block (synthetic demo, 3D hexahedra, J2 plasticity)";
    d.kind = ElemKind::Hex8;
    d.dim = 3;
    const double Ly = 2.0, Lz = 2.0, L0 = 10.0;
    auto id = [nx, ny](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
    auto mesh = [=](double L) {
        std::vector<std::array<double, 3>> X;
        for (int k = 0; k <= nz; ++k)
            for (int j = 0; j <= ny; ++j)
                for (int i = 0; i <= nx; ++i) X.push_back({L * i / nx, Ly * j / ny, Lz * k / nz});
        return X;
    };
    d.X0 = mesh(L0);
    d.geometry = mesh;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                d.conn.push_back({id(i, j, k), id(i + 1, j, k), id(i + 1, j + 1, k), id(i, j + 1, k),
                                  id(i, j, k + 1), id(i + 1, j, k + 1), id(i + 1, j + 1, k + 1), id(i, j + 1, k + 1)});
    for (int k = 0; k <= nz; ++k)
        for (int j = 0; j <= ny; ++j)
            for (int c = 0; c < 3; ++c) d.bcs.push_back({id(0, j, k), c, 0.0, false});
    const double share = 1.0 / double((ny + 1) * (nz + 1));
    for (int k = 0; k <= nz; ++k)
        for (int j = 0; j <= ny; ++j) d.loads.push_back({id(nx, j, k), 2, -share});
    d.times = {0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0};
    d.amplitude = {0.25, 0.5, 0.75, 1.0, 0.6, 0.2, 0.7, 1.1};
    d.params.push_back({ParamRole::YoungsModulus, makeParam("syn:E", "E", "Young's modulus", ParamCategory::Material, 70000.0, true)});
    d.params.push_back({ParamRole::Poisson, makeParam("syn:nu", "nu", "Poisson's ratio", ParamCategory::Material, 0.33, false)});
    d.params.back().info.lowerBound = -1.0;
    d.params.back().info.upperBound = 0.5;
    d.params.push_back({ParamRole::YieldStress, makeParam("syn:sy", "sigma_y", "Initial yield stress", ParamCategory::Material, 180.0, true)});
    d.params.push_back({ParamRole::Hardening, makeParam("syn:H", "H", "Linear isotropic hardening modulus", ParamCategory::Material, 1500.0, true)});
    d.params.push_back({ParamRole::LoadScale, makeParam("syn:F", "F_tip", "Total downward tip force", ParamCategory::Load, 140.0, false)});
    d.params.push_back({ParamRole::Geometry, makeParam("syn:L", "L_beam", "Beam length (nodes stretch, topology fixed)", ParamCategory::Geometry, L0, true)});
    d.params.back().info.mode = EditMode::Offset;
    return d;
}

}  // namespace abss::synthetic


