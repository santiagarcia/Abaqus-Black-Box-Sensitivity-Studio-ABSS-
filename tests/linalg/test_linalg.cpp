#include "abss/linalg/parallel.hpp"
#include "abss/linalg/sparse.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

using namespace abss;

namespace {
// 2D Poisson 5-point matrix (SPD) and a nonsymmetric convection-diffusion variant.
CsrMatrix laplace2d(int n, double conv = 0.0) {
    std::vector<Triplet> t;
    auto id = [n](int i, int j) { return i * n + j; };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            t.push_back({id(i, j), id(i, j), 4.0});
            if (i > 0) t.push_back({id(i, j), id(i - 1, j), -1.0 - conv});
            if (i < n - 1) t.push_back({id(i, j), id(i + 1, j), -1.0 + conv});
            if (j > 0) t.push_back({id(i, j), id(i, j - 1), -1.0});
            if (j < n - 1) t.push_back({id(i, j), id(i, j + 1), -1.0});
        }
    return CsrMatrix::fromTriplets(n * n, n * n, t);
}
}  // namespace

TEST_CASE("CSR: duplicates summed, submatrix, symmetry measure") {
    auto a = CsrMatrix::fromTriplets(3, 3, {{0, 0, 1}, {0, 0, 2}, {1, 2, 5}, {2, 1, 5}, {2, 2, 7}});
    CHECK(a.get(0, 0) == 3.0);
    CHECK(a.asymmetry() == 0.0);
    auto s = a.submatrix({1, 2}, {1, 2});
    CHECK(s.get(0, 1) == 5.0);
    CHECK(s.get(1, 1) == 7.0);
}

TEST_CASE("Every available solver solves SPD multi-RHS with one factorization; no explicit inverse") {
    const auto a = laplace2d(30);
    std::vector<std::vector<double>> rhs(5, std::vector<double>(900));
    for (int k = 0; k < 5; ++k)
        for (int i = 0; i < 900; ++i) rhs[static_cast<size_t>(k)][static_cast<size_t>(i)] = std::sin(0.01 * i * (k + 1));
    for (auto kind : availableSolvers()) {
        SolveReport rep;
        auto x = solveMany(a, rhs, kind, &rep);
        CAPTURE(solverName(kind));
        CHECK(rep.ok);
        CHECK(rep.relativeResidual < 1e-9);
    }
}

TEST_CASE("Nonsymmetric matrices: Auto selects an LU solver and solves accurately") {
    const auto a = laplace2d(25, 0.4);
    CHECK(a.asymmetry() > 0.1);
    std::vector<double> b(625, 1.0);
    SolveReport rep;
    auto x = solveMany(a, {b}, SolverKind::Auto, &rep);
    CHECK_FALSE(rep.symmetric);
    CHECK(rep.used != SolverKind::EigenLDLT);
    CHECK(rep.relativeResidual < 1e-10);
}

TEST_CASE("Kokkos kernels agree with the serial reference") {
    par::initialize();
    MESSAGE("Compute backend: " << par::backendDescription());
    const auto a = laplace2d(60);
    std::vector<double> x(3600);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::cos(0.1 * double(i));
    std::vector<double> y1, y2;
    par::spmv(a, x, y1, false);
    par::spmv(a, x, y2, true);
    for (size_t i = 0; i < y1.size(); ++i) CHECK(y1[i] == doctest::Approx(y2[i]));
    std::vector<double> c1, c2;
    par::combine({&x, &y1, &y2}, {0.5, -1.0, 1.0}, c1, false);
    par::combine({&x, &y1, &y2}, {0.5, -1.0, 1.0}, c2, true);
    for (size_t i = 0; i < c1.size(); ++i) CHECK(c1[i] == doctest::Approx(c2[i]));
    CHECK(par::dot(x, y1, false) == doctest::Approx(par::dot(x, y1, true)));
    CHECK(par::normInf(y1, false) == doctest::Approx(par::normInf(y1, true)));
}

TEST_CASE("Column colouring recovers a sparse matrix exactly from grouped column differences") {
    const auto A = laplace2d(12, 0.3);  // nonsymmetric values, symmetric pattern
    const auto pat = symmetricColumnPattern(A);
    std::int32_t nc = 0;
    const auto colour = columnColouring(pat, &nc);
    // No two columns of one colour share a row.
    for (std::int32_t r = 0; r < A.rows; ++r) {
        std::vector<int> seen(static_cast<size_t>(nc), 0);
        for (auto k = A.rowPtr[static_cast<size_t>(r)]; k < A.rowPtr[static_cast<size_t>(r) + 1]; ++k)
            CHECK(++seen[static_cast<size_t>(colour[static_cast<size_t>(A.colIdx[static_cast<size_t>(k)])])] == 1);
    }
    CHECK(nc <= 13);  // 5-point stencil: distance-2 neighbourhood of 13 nodes
    CHECK(nc < A.cols / 10);
    // Recover A from nc products A * (sum of the unit vectors of one colour).
    std::vector<Triplet> t;
    for (std::int32_t c = 0; c < nc; ++c) {
        std::vector<double> e(static_cast<size_t>(A.cols), 0.0);
        for (size_t q = 0; q < e.size(); ++q) e[q] = colour[q] == c ? 1.0 : 0.0;
        const auto y = A.multiply(e);
        for (size_t q = 0; q < e.size(); ++q)
            if (colour[q] == c)
                for (auto s : pat[q]) t.push_back({s, static_cast<std::int32_t>(q), y[static_cast<size_t>(s)]});
    }
    const auto B = CsrMatrix::fromTriplets(A.rows, A.cols, t);
    double worst = 0;
    for (std::int32_t r = 0; r < A.rows; ++r)
        for (std::int32_t c = 0; c < A.cols; ++c) worst = std::max(worst, std::abs(A.get(r, c) - B.get(r, c)));
    CHECK(worst == 0.0);
}
