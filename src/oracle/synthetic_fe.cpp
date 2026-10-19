// Synthetic FE mechanics for the demo/test oracle: small strain, J2 plasticity with linear isotropic hardening,
// backward-Euler radial return with the algorithmically consistent tangent.
#include "abss/oracle/synthetic.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <stdexcept>

namespace abss::synthetic {

namespace {

using Mat6 = Eigen::Matrix<double, 6, 6>;
using Vec6 = Eigen::Matrix<double, 6, 1>;

// Voigt order (Abaqus): 11, 22, 33, 12, 13, 23 ; strains use engineering shear.
struct MatPoint3D {
    Vec6 stress;
    Mat6 C;
    Vec6 epsP;   // engineering shear
    double alpha;
};

MatPoint3D j2Update(const Vec6& eps, const Vec6& epsPOld, double alphaOld, const ParamValues& pv, bool tangent) {
    const double E = pv.E, nu = pv.nu;
    const double G = E / (2.0 * (1.0 + nu));
    const double Kb = E / (3.0 * (1.0 - 2.0 * nu));
    Mat6 D = Mat6::Zero();
    const double lam = Kb - 2.0 * G / 3.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) D(i, j) = lam;
        D(i, i) += 2.0 * G;
        D(i + 3, i + 3) = G;
    }
    MatPoint3D r;
    r.epsP = epsPOld;
    r.alpha = alphaOld;
    const Vec6 sTr = D * (eps - epsPOld);
    const double p = (sTr(0) + sTr(1) + sTr(2)) / 3.0;
    Vec6 dev = sTr;
    for (int i = 0; i < 3; ++i) dev(i) -= p;
    const double nrm = std::sqrt(dev(0) * dev(0) + dev(1) * dev(1) + dev(2) * dev(2) +
                                 2.0 * (dev(3) * dev(3) + dev(4) * dev(4) + dev(5) * dev(5)));
    const double q = std::sqrt(1.5) * nrm;
    const double f = q - (pv.sy + pv.H * alphaOld);
    if (f <= 0.0 || nrm == 0.0) {
        r.stress = sTr;
        if (tangent) r.C = D;
        return r;
    }
    const double dg = f / (3.0 * G + pv.H);
    const Vec6 n = dev / nrm;  // tensor components
    r.stress = sTr - 2.0 * G * std::sqrt(1.5) * dg * n;
    Vec6 dEp = std::sqrt(1.5) * dg * n;
    for (int i = 3; i < 6; ++i) dEp(i) *= 2.0;  // engineering shear
    r.epsP = epsPOld + dEp;
    r.alpha = alphaOld + dg;
    if (tangent) {
        const double beta = 1.0 - 3.0 * G * dg / q;
        const double gbar = 3.0 * G / (3.0 * G + pv.H) - (1.0 - beta);
        Mat6 Idev = Mat6::Zero();
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) Idev(i, j) = (i == j ? 1.0 : 0.0) - 1.0 / 3.0;
            Idev(i + 3, i + 3) = 0.5;
        }
        Mat6 vol = Mat6::Zero();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) vol(i, j) = 1.0;
        r.C = Kb * vol + 2.0 * G * beta * Idev - 2.0 * G * gbar * (n * n.transpose());
    }
    return r;
}

// Gauss points (2-point rule) for quad/hex.
constexpr double gp = 0.57735026918962576451;

}  // namespace

Model::Model(ModelDef def) : def_(std::move(def)) {
    if (def_.times.size() != def_.amplitude.size()) throw std::invalid_argument("synthetic model: times/amplitude size");
}

int Model::pointsPerElement() const {
    switch (def_.kind) {
        case ElemKind::Truss2: return 1;
        case ElemKind::Quad4PlaneStrain: return 4;
        case ElemKind::Hex8: return 8;
    }
    return 1;
}

int Model::stressComponents() const {
    switch (def_.kind) {
        case ElemKind::Truss2: return 1;
        case ElemKind::Quad4PlaneStrain: return 4;
        case ElemKind::Hex8: return 6;
    }
    return 1;
}

ParamValues Model::values(const std::vector<double>& delta) const {
    ParamValues v = def_.base;
    for (size_t i = 0; i < def_.params.size(); ++i) {
        const double a = def_.params[i].info.nominal + (i < delta.size() ? delta[i] : 0.0);
        switch (def_.params[i].role) {
            case ParamRole::YoungsModulus: v.E = a; break;
            case ParamRole::Poisson: v.nu = a; break;
            case ParamRole::YieldStress: v.sy = a; break;
            case ParamRole::Hardening: v.H = a; break;
            case ParamRole::Area: v.area = a; break;
            case ParamRole::LoadScale: v.loadScale = a; break;
            case ParamRole::PrescribedScale: v.presScale = a; break;
            case ParamRole::Geometry: v.geom = a; break;
        }
    }
    return v;
}

std::vector<std::array<double, 3>> Model::coordinates(const ParamValues& pv) const {
    return def_.geometry ? def_.geometry(pv.geom) : def_.X0;
}

Model::State Model::initialState() const {
    State s;
    const size_t np = def_.conn.size() * static_cast<size_t>(pointsPerElement());
    s.epsP.assign(np * 6, 0.0);
    s.alpha.assign(np, 0.0);
    return s;
}

std::vector<double> Model::externalForce(int frame, const ParamValues& pv) const {
    std::vector<double> f(static_cast<size_t>(ndof()), 0.0);
    const double amp = frame <= 0 ? 0.0 : def_.amplitude.at(static_cast<size_t>(frame - 1));
    for (const auto& l : def_.loads) f[static_cast<size_t>(l.node * def_.dim + l.dof)] += pv.loadScale * amp * l.value;
    return f;
}

std::vector<std::pair<std::int32_t, double>> Model::prescribedValues(int frame, const ParamValues& pv) const {
    const double amp = frame <= 0 ? 0.0 : def_.amplitude.at(static_cast<size_t>(frame - 1));
    std::vector<std::pair<std::int32_t, double>> out;
    for (const auto& b : def_.bcs)
        out.emplace_back(b.node * def_.dim + b.dof, b.scaled ? pv.presScale * amp * b.value : b.value);
    return out;
}

Model::Eval Model::evaluate(const std::vector<double>& u, const State& prev, const ParamValues& pv, bool tangent) const {
    const auto X = coordinates(pv);
    const int dim = def_.dim;
    Eval ev;
    ev.fint.assign(static_cast<size_t>(ndof()), 0.0);
    ev.state = prev;
    const int npe = pointsPerElement();
    const int nsc = stressComponents();
    ev.stress.assign(def_.conn.size() * static_cast<size_t>(npe * nsc), 0.0);
    ev.strain.assign(ev.stress.size(), 0.0);

    for (size_t e = 0; e < def_.conn.size(); ++e) {
        const auto& cn = def_.conn[e];
        const int nn = static_cast<int>(cn.size());
        const int nedof = nn * dim;
        auto gdof = [&](int a, int i) { return cn[static_cast<size_t>(a)] * dim + i; };
        Eigen::VectorXd ue(nedof);
        for (int a = 0; a < nn; ++a)
            for (int i = 0; i < dim; ++i) ue(a * dim + i) = u[static_cast<size_t>(gdof(a, i))];
        Eigen::VectorXd fe = Eigen::VectorXd::Zero(nedof);
        Eigen::MatrixXd ke = Eigen::MatrixXd::Zero(nedof, nedof);

        if (def_.kind == ElemKind::Truss2) {
            std::array<double, 3> d{};
            double L2 = 0;
            for (int i = 0; i < dim; ++i) {
                d[static_cast<size_t>(i)] = X[static_cast<size_t>(cn[1])][static_cast<size_t>(i)] - X[static_cast<size_t>(cn[0])][static_cast<size_t>(i)];
                L2 += d[static_cast<size_t>(i)] * d[static_cast<size_t>(i)];
            }
            const double L = std::sqrt(L2);
            Eigen::RowVectorXd B(nedof);
            for (int i = 0; i < dim; ++i) {
                B(i) = -d[static_cast<size_t>(i)] / (L * L);
                B(dim + i) = d[static_cast<size_t>(i)] / (L * L);
            }
            const double eps = B * ue;
            const size_t pidx = e;
            const double epOld = prev.epsP[pidx * 6], aOld = prev.alpha[pidx];
            const double sTr = pv.E * (eps - epOld);
            const double f = std::abs(sTr) - (pv.sy + pv.H * aOld);
            double sig = sTr, Ct = pv.E;
            if (f > 0) {
                const double dg = f / (pv.E + pv.H);
                const double sgn = sTr > 0 ? 1.0 : -1.0;
                sig = sTr - pv.E * dg * sgn;
                ev.state.epsP[pidx * 6] = epOld + dg * sgn;
                ev.state.alpha[pidx] = aOld + dg;
                Ct = pv.E * pv.H / (pv.E + pv.H);
            }
            fe = pv.area * L * B.transpose() * sig;
            if (tangent) ke = pv.area * L * Ct * B.transpose() * B;
            ev.stress[pidx] = sig;
            ev.strain[pidx] = eps;
        } else {
            const bool hex = def_.kind == ElemKind::Hex8;
            static const double qn[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            static const double hn[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                            {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
            for (int ip = 0; ip < npe; ++ip) {
                double xi[3];
                if (hex) {
                    for (int k = 0; k < 3; ++k) xi[k] = gp * hn[ip][k];
                } else {
                    // Abaqus CPE4 integration-point order: (-,-), (+,-), (-,+), (+,+)
                    static const double qip[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                    xi[0] = gp * qip[ip][0];
                    xi[1] = gp * qip[ip][1];
                    xi[2] = 0;
                }
                Eigen::MatrixXd dN(dim, nn);  // dN/dxi
                for (int a = 0; a < nn; ++a) {
                    if (hex) {
                        const double* c = hn[a];
                        dN(0, a) = 0.125 * c[0] * (1 + c[1] * xi[1]) * (1 + c[2] * xi[2]);
                        dN(1, a) = 0.125 * c[1] * (1 + c[0] * xi[0]) * (1 + c[2] * xi[2]);
                        dN(2, a) = 0.125 * c[2] * (1 + c[0] * xi[0]) * (1 + c[1] * xi[1]);
                    } else {
                        const double* c = qn[a];
                        dN(0, a) = 0.25 * c[0] * (1 + c[1] * xi[1]);
                        dN(1, a) = 0.25 * c[1] * (1 + c[0] * xi[0]);
                    }
                }
                Eigen::MatrixXd Xe(nn, dim);
                for (int a = 0; a < nn; ++a)
                    for (int i = 0; i < dim; ++i) Xe(a, i) = X[static_cast<size_t>(cn[static_cast<size_t>(a)])][static_cast<size_t>(i)];
                const Eigen::MatrixXd J = dN * Xe;
                const double detJ = J.determinant();
                if (detJ <= 0) throw std::runtime_error("synthetic model: inverted element");
                const Eigen::MatrixXd dNx = J.inverse() * dN;  // dim x nn
                Eigen::MatrixXd B = Eigen::MatrixXd::Zero(6, nedof);
                for (int a = 0; a < nn; ++a) {
                    const int c0 = a * dim;
                    B(0, c0) = dNx(0, a);
                    B(1, c0 + 1) = dNx(1, a);
                    B(3, c0) = dNx(1, a);
                    B(3, c0 + 1) = dNx(0, a);
                    if (hex) {
                        B(2, c0 + 2) = dNx(2, a);
                        B(4, c0) = dNx(2, a);
                        B(4, c0 + 2) = dNx(0, a);
                        B(5, c0 + 1) = dNx(2, a);
                        B(5, c0 + 2) = dNx(1, a);
                    }
                }
                const Vec6 eps = B * ue;
                const size_t pidx = e * static_cast<size_t>(npe) + static_cast<size_t>(ip);
                Vec6 epOld;
                for (int k = 0; k < 6; ++k) epOld(k) = prev.epsP[pidx * 6 + static_cast<size_t>(k)];
                const auto mp = j2Update(eps, epOld, prev.alpha[pidx], pv, tangent);
                for (int k = 0; k < 6; ++k) ev.state.epsP[pidx * 6 + static_cast<size_t>(k)] = mp.epsP(k);
                ev.state.alpha[pidx] = mp.alpha;
                const double w = detJ;  // unit weights, unit thickness
                fe += w * B.transpose() * mp.stress;
                if (tangent) ke += w * B.transpose() * mp.C * B;
                for (int k = 0; k < nsc; ++k) {
                    ev.stress[pidx * static_cast<size_t>(nsc) + static_cast<size_t>(k)] = mp.stress(k);
                    ev.strain[pidx * static_cast<size_t>(nsc) + static_cast<size_t>(k)] = eps(k);
                }
            }
        }
        for (int a = 0; a < nedof; ++a) {
            const int ga = gdof(a / dim, a % dim);
            ev.fint[static_cast<size_t>(ga)] += fe(a);
            if (tangent)
                for (int b = 0; b < nedof; ++b)
                    if (ke(a, b) != 0.0) ev.K.push_back({ga, gdof(b / dim, b % dim), ke(a, b)});
        }
    }
    return ev;
}

}  // namespace abss::synthetic
