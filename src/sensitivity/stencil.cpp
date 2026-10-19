#include "abss/sensitivity/engine.hpp"

#include <stdexcept>

namespace abss {

// Fornberg (1988): weights for the k-th derivative at 0 from samples at integer offsets (unit spacing).
std::vector<double> stencilWeights(const std::vector<int>& offsets, int k) {
    const int n = static_cast<int>(offsets.size());
    if (k < 0 || k >= n) throw std::invalid_argument("stencilWeights: need more points than the derivative order");
    std::vector<std::vector<double>> c(static_cast<size_t>(n), std::vector<double>(static_cast<size_t>(k) + 1, 0.0));
    double c1 = 1.0;
    double c4 = offsets[0];
    c[0][0] = 1.0;
    for (int i = 1; i < n; ++i) {
        const int mn = std::min(i, k);
        double c2 = 1.0;
        const double c5 = c4;
        c4 = offsets[static_cast<size_t>(i)];
        for (int j = 0; j < i; ++j) {
            const double c3 = offsets[static_cast<size_t>(i)] - offsets[static_cast<size_t>(j)];
            c2 *= c3;
            if (j == i - 1) {
                for (int s = mn; s >= 1; --s)
                    c[static_cast<size_t>(i)][static_cast<size_t>(s)] =
                        c1 * (s * c[static_cast<size_t>(i - 1)][static_cast<size_t>(s - 1)] -
                              c5 * c[static_cast<size_t>(i - 1)][static_cast<size_t>(s)]) / c2;
                c[static_cast<size_t>(i)][0] = -c1 * c5 * c[static_cast<size_t>(i - 1)][0] / c2;
            }
            for (int s = mn; s >= 1; --s)
                c[static_cast<size_t>(j)][static_cast<size_t>(s)] =
                    (c4 * c[static_cast<size_t>(j)][static_cast<size_t>(s)] - s * c[static_cast<size_t>(j)][static_cast<size_t>(s - 1)]) / c3;
            c[static_cast<size_t>(j)][0] = c4 * c[static_cast<size_t>(j)][0] / c3;
        }
        c1 = c2;
    }
    std::vector<double> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[static_cast<size_t>(i)] = c[static_cast<size_t>(i)][static_cast<size_t>(k)];
    return w;
}

double defaultRelativeStep(int maxOrder) { return maxOrder <= 1 ? 1e-5 : maxOrder == 2 ? 1e-4 : 1e-3; }

int defaultHalfWidth(int maxOrder) { return maxOrder <= 2 ? 1 : (maxOrder + 1) / 2; }

long long estimateOracleEvaluations(int frames, int params, int maxOrder, int halfWidth, bool tangentCheck) {
    const int M = halfWidth > 0 ? halfWidth : defaultHalfWidth(maxOrder);
    const long long offBranches = 2LL * M;
    // per frame: baseline probe + baseline commit + per parameter (K probes on each off-branch + commit each)
    const long long perFrame = 2 + static_cast<long long>(params) * offBranches * (maxOrder + 1) + (tangentCheck ? 2 : 0);
    return perFrame * frames;
}

}  // namespace abss
