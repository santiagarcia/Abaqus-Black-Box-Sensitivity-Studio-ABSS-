#include "abss/linalg/sparse.hpp"

#include "abss/linalg/parallel.hpp"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#ifdef ABSS_HAVE_SUITESPARSE
#include <umfpack.h>
#endif

namespace abss {

namespace {
using SpMat = Eigen::SparseMatrix<double, Eigen::ColMajor, int>;
double seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

SpMat toEigen(const CsrMatrix& a) {
    std::vector<Eigen::Triplet<double, int>> t;
    t.reserve(static_cast<size_t>(a.nnz()));
    for (std::int32_t r = 0; r < a.rows; ++r)
        for (std::int64_t p = a.rowPtr[static_cast<size_t>(r)]; p < a.rowPtr[static_cast<size_t>(r) + 1]; ++p)
            t.emplace_back(r, a.colIdx[static_cast<size_t>(p)], a.values[static_cast<size_t>(p)]);
    SpMat m(a.rows, a.cols);
    m.setFromTriplets(t.begin(), t.end());
    m.makeCompressed();
    return m;
}

class EigenLUFact final : public Factorization {
public:
    explicit EigenLUFact(const CsrMatrix& a) : m_(toEigen(a)) {
        lu_.analyzePattern(m_);
        lu_.factorize(m_);
        ok_ = lu_.info() == Eigen::Success;
        msg_ = ok_ ? "Eigen SparseLU" : "Eigen SparseLU failed: " + lu_.lastErrorMessage();
    }
    bool ok() const override { return ok_; }
    std::string message() const override { return msg_; }
    void solve(std::vector<double>& block, int nrhs) const override {
        Eigen::Map<Eigen::MatrixXd> b(block.data(), m_.rows(), nrhs);
        Eigen::MatrixXd x = lu_.solve(b);
        b = x;
    }

private:
    SpMat m_;
    Eigen::SparseLU<SpMat, Eigen::COLAMDOrdering<int>> lu_;
    bool ok_ = false;
    std::string msg_;
};

class EigenLDLTFact final : public Factorization {
public:
    explicit EigenLDLTFact(const CsrMatrix& a) : m_(toEigen(a)) {
        ldlt_.compute(m_);
        ok_ = ldlt_.info() == Eigen::Success;
        msg_ = ok_ ? "Eigen SimplicialLDLT" : "Eigen SimplicialLDLT failed (matrix not symmetric/indefinite?)";
    }
    bool ok() const override { return ok_; }
    std::string message() const override { return msg_; }
    void solve(std::vector<double>& block, int nrhs) const override {
        Eigen::Map<Eigen::MatrixXd> b(block.data(), m_.rows(), nrhs);
        Eigen::MatrixXd x = ldlt_.solve(b);
        b = x;
    }

private:
    SpMat m_;
    Eigen::SimplicialLDLT<SpMat> ldlt_;
    bool ok_ = false;
    std::string msg_;
};

class DenseLUFact final : public Factorization {
public:
    explicit DenseLUFact(const CsrMatrix& a) : lu_(Eigen::MatrixXd(toEigen(a))) {
        ok_ = lu_.rank() == a.rows;
    }
    bool ok() const override { return ok_; }
    std::string message() const override { return ok_ ? "Dense full-pivot LU" : "Dense LU: matrix is singular"; }
    void solve(std::vector<double>& block, int nrhs) const override {
        Eigen::Map<Eigen::MatrixXd> b(block.data(), lu_.rows(), nrhs);
        Eigen::MatrixXd x = lu_.solve(b);
        b = x;
    }

private:
    Eigen::FullPivLU<Eigen::MatrixXd> lu_;
    bool ok_ = false;
};

class KokkosCGFact final : public Factorization {
public:
    explicit KokkosCGFact(const CsrMatrix& a) : a_(a) {}
    bool ok() const override { return true; }
    std::string message() const override { return "Jacobi-PCG (" + par::backendDescription() + ")"; }
    void solve(std::vector<double>& block, int nrhs) const override {
        const size_t n = static_cast<size_t>(a_.rows);
        for (int k = 0; k < nrhs; ++k) {
            std::vector<double> b(block.begin() + static_cast<std::ptrdiff_t>(k * n),
                                  block.begin() + static_cast<std::ptrdiff_t>((k + 1) * n));
            std::vector<double> x(n, 0.0);
            if (par::conjugateGradient(a_, b, x, 1e-13, static_cast<int>(10 * n + 100)) < 0)
                throw std::runtime_error("Kokkos PCG did not converge (matrix not SPD or ill-conditioned)");
            std::copy(x.begin(), x.end(), block.begin() + static_cast<std::ptrdiff_t>(k * n));
        }
    }

private:
    CsrMatrix a_;
};

#ifdef ABSS_HAVE_SUITESPARSE
class UmfpackFact final : public Factorization {
public:
    explicit UmfpackFact(const CsrMatrix& a) : n_(a.rows) {
        // UMFPACK wants CSC; factor A^T from CSR arrays then solve with the transpose system flag.
        ap_.assign(a.rowPtr.begin(), a.rowPtr.end());
        ai_.assign(a.colIdx.begin(), a.colIdx.end());
        ax_ = a.values;
        void* sym = nullptr;
        int st = umfpack_dl_symbolic(n_, n_, ap_.data(), ai_.data(), ax_.data(), &sym, nullptr, nullptr);
        if (st == UMFPACK_OK) st = umfpack_dl_numeric(ap_.data(), ai_.data(), ax_.data(), sym, &num_, nullptr, nullptr);
        if (sym) umfpack_dl_free_symbolic(&sym);
        ok_ = st == UMFPACK_OK;
        msg_ = ok_ ? "SuiteSparse UMFPACK" : "UMFPACK failed with status " + std::to_string(st);
    }
    ~UmfpackFact() override {
        if (num_) umfpack_dl_free_numeric(&num_);
    }
    bool ok() const override { return ok_; }
    std::string message() const override { return msg_; }
    void solve(std::vector<double>& block, int nrhs) const override {
        std::vector<double> x(static_cast<size_t>(n_));
        for (int k = 0; k < nrhs; ++k) {
            double* b = block.data() + static_cast<size_t>(k) * n_;
            // Stored matrix is A^T in CSC form; UMFPACK_At solves (A^T)^T x = A x = b.
            int st = umfpack_dl_solve(UMFPACK_At, ap_.data(), ai_.data(), ax_.data(), x.data(), b, num_, nullptr, nullptr);
            if (st != UMFPACK_OK) throw std::runtime_error("UMFPACK solve failed");
            std::copy(x.begin(), x.end(), b);
        }
    }

private:
    std::int64_t n_;
    std::vector<std::int64_t> ap_, ai_;
    std::vector<double> ax_;
    void* num_ = nullptr;
    bool ok_ = false;
    std::string msg_;
};
#endif

}  // namespace

const char* solverName(SolverKind k) {
    switch (k) {
        case SolverKind::Auto: return "Automatic";
        case SolverKind::EigenSparseLU: return "Sparse LU (Eigen)";
        case SolverKind::EigenLDLT: return "Sparse LDLT (Eigen)";
        case SolverKind::Umfpack: return "UMFPACK (SuiteSparse)";
        case SolverKind::KokkosCG: return "Jacobi-PCG (Kokkos)";
        case SolverKind::DenseLU: return "Dense LU (small/validation)";
    }
    return "?";
}

std::vector<SolverKind> availableSolvers() {
    std::vector<SolverKind> s{SolverKind::EigenSparseLU, SolverKind::EigenLDLT};
#ifdef ABSS_HAVE_SUITESPARSE
    s.push_back(SolverKind::Umfpack);
#endif
    s.push_back(SolverKind::KokkosCG);
    s.push_back(SolverKind::DenseLU);
    return s;
}

CsrMatrix CsrMatrix::fromTriplets(std::int32_t rows, std::int32_t cols, std::vector<Triplet> t) {
    std::sort(t.begin(), t.end(), [](const Triplet& a, const Triplet& b) {
        return a.row != b.row ? a.row < b.row : a.col < b.col;
    });
    CsrMatrix m;
    m.rows = rows;
    m.cols = cols;
    m.rowPtr.assign(static_cast<size_t>(rows) + 1, 0);
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].row < 0 || t[i].row >= rows || t[i].col < 0 || t[i].col >= cols)
            throw std::out_of_range("CsrMatrix::fromTriplets: index out of range");
        if (!m.colIdx.empty() && i > 0 && t[i].row == t[i - 1].row && t[i].col == t[i - 1].col) {
            m.values.back() += t[i].value;
            continue;
        }
        m.colIdx.push_back(t[i].col);
        m.values.push_back(t[i].value);
        m.rowPtr[static_cast<size_t>(t[i].row) + 1]++;
    }
    for (std::int32_t r = 0; r < rows; ++r) m.rowPtr[static_cast<size_t>(r) + 1] += m.rowPtr[static_cast<size_t>(r)];
    return m;
}

void CsrMatrix::multiply(const std::vector<double>& x, std::vector<double>& y) const { par::spmv(*this, x, y, true); }

std::vector<double> CsrMatrix::multiply(const std::vector<double>& x) const {
    std::vector<double> y;
    multiply(x, y);
    return y;
}

CsrMatrix CsrMatrix::submatrix(const std::vector<std::int32_t>& rowsSel, const std::vector<std::int32_t>& colsSel) const {
    std::vector<std::int32_t> colMap(static_cast<size_t>(cols), -1);
    for (size_t j = 0; j < colsSel.size(); ++j) colMap[static_cast<size_t>(colsSel[j])] = static_cast<std::int32_t>(j);
    std::vector<Triplet> t;
    for (size_t i = 0; i < rowsSel.size(); ++i) {
        const auto r = static_cast<size_t>(rowsSel[i]);
        for (std::int64_t p = rowPtr[r]; p < rowPtr[r + 1]; ++p) {
            const auto c = colMap[static_cast<size_t>(colIdx[static_cast<size_t>(p)])];
            if (c >= 0) t.push_back({static_cast<std::int32_t>(i), c, values[static_cast<size_t>(p)]});
        }
    }
    return fromTriplets(static_cast<std::int32_t>(rowsSel.size()), static_cast<std::int32_t>(colsSel.size()), std::move(t));
}

double CsrMatrix::get(std::int32_t r, std::int32_t c) const {
    const auto b = colIdx.begin() + rowPtr[static_cast<size_t>(r)];
    const auto e = colIdx.begin() + rowPtr[static_cast<size_t>(r) + 1];
    auto it = std::lower_bound(b, e, c);
    return (it != e && *it == c) ? values[static_cast<size_t>(it - colIdx.begin())] : 0.0;
}

double CsrMatrix::maxAbs() const {
    double m = 0.0;
    for (double v : values) m = std::max(m, std::abs(v));
    return m;
}

double CsrMatrix::asymmetry() const {
    const double scale = maxAbs();
    if (scale == 0.0 || rows != cols) return rows == cols ? 0.0 : 1.0;
    double d = 0.0;
    for (std::int32_t r = 0; r < rows; ++r)
        for (std::int64_t p = rowPtr[static_cast<size_t>(r)]; p < rowPtr[static_cast<size_t>(r) + 1]; ++p)
            d = std::max(d, std::abs(values[static_cast<size_t>(p)] - get(colIdx[static_cast<size_t>(p)], r)));
    return d / scale;
}

std::unique_ptr<Factorization> factorize(const CsrMatrix& a, SolverKind kind, SolveReport* report) {
    if (a.rows != a.cols) throw std::invalid_argument("factorize: matrix is not square");
    const auto t0 = std::chrono::steady_clock::now();
    const bool sym = a.asymmetry() < 1e-10;
    if (kind == SolverKind::Auto) {
#ifdef ABSS_HAVE_SUITESPARSE
        kind = sym ? SolverKind::EigenLDLT : SolverKind::Umfpack;
#else
        kind = sym ? SolverKind::EigenLDLT : SolverKind::EigenSparseLU;
#endif
    }
    std::unique_ptr<Factorization> f;
    switch (kind) {
        case SolverKind::EigenLDLT: f = std::make_unique<EigenLDLTFact>(a); break;
        case SolverKind::EigenSparseLU: f = std::make_unique<EigenLUFact>(a); break;
        case SolverKind::DenseLU: f = std::make_unique<DenseLUFact>(a); break;
        case SolverKind::KokkosCG: f = std::make_unique<KokkosCGFact>(a); break;
        case SolverKind::Umfpack:
#ifdef ABSS_HAVE_SUITESPARSE
            f = std::make_unique<UmfpackFact>(a);
            break;
#else
            throw std::runtime_error("UMFPACK was not compiled into this build");
#endif
        case SolverKind::Auto: break;
    }
    // Indefinite symmetric tangents (e.g. after softening) can break LDLT: fall back to LU.
    if (!f->ok() && kind == SolverKind::EigenLDLT) {
        kind = SolverKind::EigenSparseLU;
        f = std::make_unique<EigenLUFact>(a);
    }
    if (report) {
        report->used = kind;
        report->symmetric = sym;
        report->factorSeconds = seconds(t0);
        report->ok = f->ok();
        report->message = f->message();
    }
    return f;
}

std::vector<std::vector<double>> solveMany(const CsrMatrix& a, const std::vector<std::vector<double>>& rhs,
                                           SolverKind kind, SolveReport* report) {
    SolveReport local;
    SolveReport& rep = report ? *report : local;
    auto f = factorize(a, kind, &rep);
    if (!f->ok()) throw std::runtime_error("Linear solver failed: " + f->message());
    const size_t n = static_cast<size_t>(a.rows);
    const int nrhs = static_cast<int>(rhs.size());
    std::vector<double> block(n * rhs.size());
    for (size_t k = 0; k < rhs.size(); ++k) {
        if (rhs[k].size() != n) throw std::invalid_argument("solveMany: RHS size mismatch");
        std::copy(rhs[k].begin(), rhs[k].end(), block.begin() + static_cast<std::ptrdiff_t>(k * n));
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (nrhs > 0) f->solve(block, nrhs);
    rep.solveSeconds = seconds(t0);
    std::vector<std::vector<double>> out(rhs.size());
    rep.relativeResidual = 0.0;
    for (size_t k = 0; k < rhs.size(); ++k) {
        out[k].assign(block.begin() + static_cast<std::ptrdiff_t>(k * n), block.begin() + static_cast<std::ptrdiff_t>((k + 1) * n));
        const auto ax = a.multiply(out[k]);
        double rn = 0, bn = 0;
        for (size_t i = 0; i < n; ++i) {
            rn = std::max(rn, std::abs(ax[i] - rhs[k][i]));
            bn = std::max(bn, std::abs(rhs[k][i]));
        }
        rep.relativeResidual = std::max(rep.relativeResidual, bn > 0 ? rn / bn : rn);
    }
    return out;
}

}  // namespace abss
