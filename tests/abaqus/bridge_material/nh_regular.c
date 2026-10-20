#include "nh_regular.h"

#include <math.h>
#include <stdio.h>

void nh_reg_sdvini(int n, int nstatv, double* statev, const double* coords, const int* noel, const int* npt) {
    (void)coords, (void)noel, (void)npt;
    for (int i = 0; i < n; ++i) {
        statev[i * nstatv + 0] = 42.0;
        statev[i * nstatv + 1] = 1.0;
    }
    printf(" nh_reg_sdvini: %d point(s) initialised\n", n); /* vendor libraries print too; must not break the bridge */
}

void nh_regular(int n, int nprops, int ntens, int nstatv, const double* props, double* stress, double* statev,
                double* ddsdde, const double* f0, const double* f1, double dtime, double time1, double temp,
                double dtemp, const double* coords, const double* celent, const int* noel, const int* npt, int kstep,
                int kinc, double* pnewdt) {
    (void)nprops, (void)f0, (void)dtime, (void)time1, (void)temp, (void)dtemp, (void)coords, (void)celent, (void)noel,
        (void)npt, (void)kstep, (void)kinc;
    const double mu = props[0], lam = props[1];
    static const int vi[6] = {0, 1, 2, 0, 0, 1}, vj[6] = {0, 1, 2, 1, 2, 2};
    for (int p = 0; p < n; ++p) {
        const double* F = f1 + 9 * p; /* Fortran order: F(a,b) = F[a + 3 b] */
        double B[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                B[a][b] = 0;
                for (int k = 0; k < 3; ++k) B[a][b] += F[a + 3 * k] * F[b + 3 * k];
            }
        const double J = F[0] * (F[4] * F[8] - F[7] * F[5]) - F[3] * (F[1] * F[8] - F[7] * F[2]) +
                         F[6] * (F[1] * F[5] - F[4] * F[2]);
        double* st = statev + p * nstatv;
        if (st[0] != 42.0 || J <= 0) { /* not initialised by SDVINI, or inverted element: refuse the increment */
            pnewdt[p] = 0.5;
            continue;
        }
        double* s = stress + p * ntens;
        for (int c = 0; c < 6; ++c) {
            const int a = vi[c], b = vj[c];
            s[c] = mu / J * (B[a][b] - (a == b)) + (a == b) * lam * log(J) / J;
        }
        double* D = ddsdde + p * ntens * ntens;
#ifdef NH_APPROX_TANGENT
        /* Approximate tangent (no geometric terms): exercises ABSS's inconsistent-tangent (GMRES) path. */
        const double g = (mu - lam * log(J)) / J;
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c) D[r + ntens * c] = 0.0;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) D[r + ntens * c] = lam / J;
            D[r + ntens * r] += 2.0 * g;
            D[(r + 3) + ntens * (r + 3)] = g;
        }
#else
        /* Abaqus finite-strain convention: DDSDDE = (1/J) c^(tau, Jaumann), engineering shear strains.
         * c^(tau,J)_ijkl = lam d_ij d_kl + (mu - lam lnJ)(d_ik d_jl + d_il d_jk) + 1/2 (d_ik t_jl + t_ik d_jl + d_il t_jk + t_il d_jk),
         * with t = J sigma (Kirchhoff stress). */
        double tau[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) tau[a][b] = mu * (B[a][b] - (a == b)) + (a == b) * lam * log(J);
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c) {
                const int i = vi[r], j = vj[r], k = vi[c], l = vj[c];
                const double dik = i == k, djl = j == l, dil = i == l, djk = j == k;
                const double v = lam * (i == j) * (k == l) + (mu - lam * log(J)) * (dik * djl + dil * djk) +
                                 0.5 * (dik * tau[j][l] + tau[i][k] * djl + dil * tau[j][k] + tau[i][l] * djk);
                D[r + ntens * c] = v / J;
            }
#endif
        st[1] = J;
        pnewdt[p] = 1.5;
    }
}
