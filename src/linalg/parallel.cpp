#include "abss/linalg/parallel.hpp"

#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

#ifdef ABSS_HAVE_KOKKOS
#include <Kokkos_Core.hpp>
#endif

namespace abss::par {

#ifdef ABSS_HAVE_KOKKOS
namespace {
std::once_flag gInit;
using Host = Kokkos::DefaultHostExecutionSpace;
template <class T>
using Unmanaged = Kokkos::View<T*, Kokkos::HostSpace, Kokkos::MemoryTraits<Kokkos::Unmanaged>>;
}  // namespace

void initialize() {
    std::call_once(gInit, [] {
        if (!Kokkos::is_initialized()) {
            Kokkos::InitializationSettings s;
            s.set_disable_warnings(true);
            Kokkos::initialize(s);
            std::atexit([] {
                if (Kokkos::is_initialized() && !Kokkos::is_finalized()) Kokkos::finalize();
            });
        }
    });
}
bool kokkosEnabled() { return true; }
std::string backendDescription() {
    initialize();
    return std::string("Kokkos ") + std::to_string(KOKKOS_VERSION / 10000) + "." +
           std::to_string(KOKKOS_VERSION / 100 % 100) + "." + std::to_string(KOKKOS_VERSION % 100) + " " +
           Host::name() + " (" + std::to_string(Host().concurrency()) + " threads)";
}
#else
void initialize() {}
bool kokkosEnabled() { return false; }
std::string backendDescription() { return "Serial CPU fallback (Kokkos not built)"; }
#endif

void combine(const std::vector<const std::vector<double>*>& in, const std::vector<double>& w, std::vector<double>& out,
             bool forceSerial) {
    if (in.size() != w.size()) throw std::invalid_argument("combine: weights/arrays mismatch");
    const size_t n = in.empty() ? 0 : in.front()->size();
    for (auto* v : in)
        if (v->size() != n) throw std::invalid_argument("combine: array size mismatch");
    out.assign(n, 0.0);
#ifdef ABSS_HAVE_KOKKOS
    if (!forceSerial) {
        initialize();
        std::vector<const double*> ptr;
        for (auto* v : in) ptr.push_back(v->data());
        const double* const* pp = ptr.data();
        const double* ww = w.data();
        double* o = out.data();
        const int m = static_cast<int>(in.size());
        Kokkos::parallel_for(
            "abss::combine", Kokkos::RangePolicy<Host>(0, n), KOKKOS_LAMBDA(const size_t i) {
                double s = 0.0;
                for (int k = 0; k < m; ++k) s += ww[k] * pp[k][i];
                o[i] = s;
            });
        Kokkos::fence();
        return;
    }
#endif
    for (size_t k = 0; k < in.size(); ++k) {
        const double wk = w[k];
        const double* p = in[k]->data();
        for (size_t i = 0; i < n; ++i) out[i] += wk * p[i];
    }
}

void spmv(const CsrMatrix& a, const std::vector<double>& x, std::vector<double>& y, bool forceSerial) {
    spmm(a, x, 1, y, forceSerial);
}

void spmm(const CsrMatrix& a, const std::vector<double>& x, int nrhs, std::vector<double>& y, bool forceSerial) {
    if (static_cast<std::int64_t>(x.size()) != static_cast<std::int64_t>(a.cols) * nrhs)
        throw std::invalid_argument("spmm: dimension mismatch");
    y.assign(static_cast<size_t>(a.rows) * nrhs, 0.0);
    const std::int64_t* rp = a.rowPtr.data();
    const std::int32_t* ci = a.colIdx.data();
    const double* av = a.values.data();
    const double* xv = x.data();
    double* yv = y.data();
    const std::int32_t nr = a.rows, nc = a.cols;
#ifdef ABSS_HAVE_KOKKOS
    if (!forceSerial) {
        initialize();
        Kokkos::parallel_for(
            "abss::spmm", Kokkos::RangePolicy<Host>(0, nr), KOKKOS_LAMBDA(const std::int32_t r) {
                for (int k = 0; k < nrhs; ++k) {
                    double s = 0.0;
                    for (std::int64_t p = rp[r]; p < rp[r + 1]; ++p) s += av[p] * xv[static_cast<size_t>(k) * nc + ci[p]];
                    yv[static_cast<size_t>(k) * nr + r] = s;
                }
            });
        Kokkos::fence();
        return;
    }
#endif
    for (std::int32_t r = 0; r < nr; ++r)
        for (int k = 0; k < nrhs; ++k) {
            double s = 0.0;
            for (std::int64_t p = rp[r]; p < rp[r + 1]; ++p) s += av[p] * xv[static_cast<size_t>(k) * nc + ci[p]];
            yv[static_cast<size_t>(k) * nr + r] = s;
        }
}

double dot(const std::vector<double>& a, const std::vector<double>& b, bool forceSerial) {
    const size_t n = a.size();
#ifdef ABSS_HAVE_KOKKOS
    if (!forceSerial) {
        initialize();
        const double* x = a.data();
        const double* y = b.data();
        double s = 0.0;
        Kokkos::parallel_reduce(
            "abss::dot", Kokkos::RangePolicy<Host>(0, n), KOKKOS_LAMBDA(const size_t i, double& acc) { acc += x[i] * y[i]; },
            s);
        return s;
    }
#endif
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

double normInf(const std::vector<double>& a, bool forceSerial) {
#ifdef ABSS_HAVE_KOKKOS
    if (!forceSerial) {
        initialize();
        const double* x = a.data();
        double m = 0.0;
        Kokkos::parallel_reduce(
            "abss::normInf", Kokkos::RangePolicy<Host>(0, a.size()),
            KOKKOS_LAMBDA(const size_t i, double& acc) {
                const double v = x[i] < 0 ? -x[i] : x[i];
                if (v > acc) acc = v;
            },
            Kokkos::Max<double>(m));
        return m;
    }
#endif
    double m = 0.0;
    for (double v : a) m = std::max(m, std::abs(v));
    return m;
}

int conjugateGradient(const CsrMatrix& a, const std::vector<double>& b, std::vector<double>& x, double relTol,
                      int maxIter, bool forceSerial) {
    const size_t n = static_cast<size_t>(a.rows);
    if (x.size() != n) x.assign(n, 0.0);
    std::vector<double> diag(n, 1.0);
    for (std::int32_t r = 0; r < a.rows; ++r) {
        const double d = a.get(r, r);
        diag[static_cast<size_t>(r)] = d != 0.0 ? 1.0 / d : 1.0;
    }
    std::vector<double> r(n), z(n), p(n), q;
    spmv(a, x, q, forceSerial);
    for (size_t i = 0; i < n; ++i) r[i] = b[i] - q[i];
    const double bn = std::sqrt(dot(b, b, forceSerial));
    if (bn == 0.0) {
        x.assign(n, 0.0);
        return 0;
    }
    for (size_t i = 0; i < n; ++i) z[i] = diag[i] * r[i];
    p = z;
    double rz = dot(r, z, forceSerial);
    for (int it = 1; it <= maxIter; ++it) {
        spmv(a, p, q, forceSerial);
        const double alpha = rz / dot(p, q, forceSerial);
        for (size_t i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * q[i];
        }
        if (std::sqrt(dot(r, r, forceSerial)) <= relTol * bn) return it;
        for (size_t i = 0; i < n; ++i) z[i] = diag[i] * r[i];
        const double rzNew = dot(r, z, forceSerial);
        const double beta = rzNew / rz;
        rz = rzNew;
        for (size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    }
    return -1;
}

}  // namespace abss::par
