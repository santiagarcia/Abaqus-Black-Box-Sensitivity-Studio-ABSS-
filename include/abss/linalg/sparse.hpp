// ABSS sparse linear algebra: CSR storage, DOF sub-blocking and factorization reuse. No explicit inverses.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace abss {

struct Triplet {
    std::int32_t row = 0;
    std::int32_t col = 0;
    double value = 0.0;
};

/// Compressed sparse row matrix (0-based). Duplicate triplets are summed on construction.
class CsrMatrix {
public:
    std::int32_t rows = 0;
    std::int32_t cols = 0;
    std::vector<std::int64_t> rowPtr;  ///< size rows+1
    std::vector<std::int32_t> colIdx;
    std::vector<double> values;

    static CsrMatrix fromTriplets(std::int32_t rows, std::int32_t cols, std::vector<Triplet> t);
    std::int64_t nnz() const { return static_cast<std::int64_t>(values.size()); }

    /// y = A x
    void multiply(const std::vector<double>& x, std::vector<double>& y) const;
    std::vector<double> multiply(const std::vector<double>& x) const;
    /// Extract A(rowsSel, colsSel) (index lists into this matrix).
    CsrMatrix submatrix(const std::vector<std::int32_t>& rowsSel, const std::vector<std::int32_t>& colsSel) const;
    /// max |A - A^T| / max |A|
    double asymmetry() const;
    double maxAbs() const;
    double get(std::int32_t r, std::int32_t c) const;
};

/// Rows that may be nonzero in each column of a structurally symmetric matrix: pattern(A) united with pattern(A^T),
/// plus the diagonal. Index lists are sorted.
std::vector<std::vector<std::int32_t>> symmetricColumnPattern(const CsrMatrix& a);

/// Curtis-Powell-Reid column grouping: greedy distance-2 colouring of the pattern so that no two columns of one colour
/// share a row. Perturbing all columns of a colour together and differencing recovers each of their entries exactly
/// (sparse Jacobian estimation). Returns the colour of every column (0-based); *nColours is set when given.
std::vector<std::int32_t> columnColouring(const std::vector<std::vector<std::int32_t>>& pattern, std::int32_t* nColours = nullptr);

enum class SolverKind : std::uint8_t { Auto, EigenSparseLU, EigenLDLT, Umfpack, KokkosCG, DenseLU };
const char* solverName(SolverKind k);

/// Which sparse solvers were compiled in (capability detection).
std::vector<SolverKind> availableSolvers();

struct SolveReport {
    SolverKind used = SolverKind::Auto;
    double relativeResidual = -1.0;  ///< max over RHS of ||A x - b|| / ||b||
    double factorSeconds = 0.0;
    double solveSeconds = 0.0;
    bool symmetric = false;
    bool ok = false;
    std::string message;
};

/// A factorization of one matrix, reused for many right-hand sides (all parameters/orders at a frame).
class Factorization {
public:
    virtual ~Factorization() = default;
    virtual bool ok() const = 0;
    virtual std::string message() const = 0;
    /// Solve A X = B for column-major RHS block B (n x nrhs); overwrites with X.
    virtual void solve(std::vector<double>& block, int nrhs) const = 0;
};

/// Factor A. Auto picks LDLT/Cholesky for (numerically) symmetric matrices, LU otherwise.
std::unique_ptr<Factorization> factorize(const CsrMatrix& a, SolverKind kind, SolveReport* report);

/// Convenience: factor once, solve every RHS, verify residuals.
std::vector<std::vector<double>> solveMany(const CsrMatrix& a, const std::vector<std::vector<double>>& rhs,
                                           SolverKind kind, SolveReport* report);

}  // namespace abss
