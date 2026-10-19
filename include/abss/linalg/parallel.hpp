// ABSS parallel kernels: Kokkos when available (OpenMP/CUDA/HIP per installed build), serial fallback otherwise.
// Every kernel has a serial reference so speedups are measured, not assumed.
#pragma once

#include "abss/linalg/sparse.hpp"

#include <string>
#include <vector>

namespace abss::par {

/// Initialize the compute backend once per process (safe to call repeatedly). Finalized at exit.
void initialize();
bool kokkosEnabled();
/// e.g. "Kokkos 5.2.2 OpenMP (8 threads)" or "Serial CPU fallback (Kokkos not built)".
std::string backendDescription();

/// out[i] = sum_m w[m] * in[m][i]  (finite-difference stencil applied to many large arrays)
void combine(const std::vector<const std::vector<double>*>& in, const std::vector<double>& w, std::vector<double>& out,
             bool forceSerial = false);
/// y = A x
void spmv(const CsrMatrix& a, const std::vector<double>& x, std::vector<double>& y, bool forceSerial = false);
/// Y = A X for column-major block X (n x nrhs)
void spmm(const CsrMatrix& a, const std::vector<double>& x, int nrhs, std::vector<double>& y, bool forceSerial = false);
double dot(const std::vector<double>& a, const std::vector<double>& b, bool forceSerial = false);
double normInf(const std::vector<double>& a, bool forceSerial = false);

/// Jacobi-preconditioned conjugate gradients (SPD only). Returns iterations; -1 if not converged.
int conjugateGradient(const CsrMatrix& a, const std::vector<double>& b, std::vector<double>& x, double relTol,
                      int maxIter, bool forceSerial = false);

}  // namespace abss::par
