/* Test material for the ABSS material bridge: compressible neo-Hookean, driven by the deformation gradient, with the
 * same calling convention as a vendor "regular" update (batched over n points, Fortran-order arrays). */
#ifndef NH_REGULAR_H
#define NH_REGULAR_H

#define NH_NTENS 6
#define NH_NSTATV 2
#define NH_NPROPS 2

#ifdef _WIN32
#define NH_API __declspec(dllexport)
#else
#define NH_API
#endif

/* statev[0] = 42 marks an initialised point; statev[1] = J after the last update. */
NH_API void nh_reg_sdvini(int n, int nstatv, double* statev, const double* coords, const int* noel, const int* npt);

/* props = {mu, lambda}. sigma = mu/J (B - I) + lambda ln(J)/J I. */
NH_API void nh_regular(int n, int nprops, int ntens, int nstatv, const double* props, double* stress, double* statev,
                       double* ddsdde, const double* f0, const double* f1, double dtime, double time1, double temp,
                       double dtemp, const double* coords, const double* celent, const int* noel, const int* npt,
                       int kstep, int kinc, double* pnewdt);

#endif
