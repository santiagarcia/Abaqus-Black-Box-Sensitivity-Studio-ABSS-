# -*- coding: utf-8 -*-
"""E3: oracle-based first-order sensitivity and predictor-consistency test.

    abaqus python e3_predictor.py

Model: the E1 CPS4 patch (linear elastic, NLGEOM off), parameters
  a = E (Young's modulus, material)       -> s_C = 0
  b = prescribed U2 at node 3 (BC value)  -> s_C = e_b  (nonzero BC sensitivity)
Oracle: every DOF prescribed, original loads kept, RF read (double precision).
  r = F_ext - F_int = -RF (verified in E1).  K = -dr/du = dRF/du.
Steps:
  1. nominal u*; 2. K_FF, K_FC from oracle FD columns (black box) and compare
  with analytic K; 3. p_E = [r(u*,E+h) - r(u*,E)]/h; 4. solve
  K_FF s_F = p_F - K_FC s_C; 5. compare with central FD re-solves and with the
  analytic derivative; 6. predictor residual |r_F(u*+h s, E+h)| vs h.
Writes results/e3_predictor.json.
"""
from __future__ import print_function

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402
import e1_oracle as P  # noqa: E402  (patch model definition)

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work", "e3")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]
NN = 9
E0 = P.PATCH_E
B0 = 0.01  # nominal prescribed U2 at node 3


def bcs_nominal(b=B0):
    return [(1, 1, 0.0), (1, 2, 0.0), (4, 1, 0.0), (7, 1, 0.0), (3, 2, b)]


CONS = sorted(2 * (n - 1) + d - 1 for n, d, v in bcs_nominal())
FREE = [k for k in range(2 * NN) if k not in CONS]
IB = 2 * (3 - 1) + 1  # DOF index of the BC parameter


def solve(name, E=E0, b=B0):
    T.run_job(WORK, name, P.deck(P.patch_model_data(E), bcs_nominal(b), P.PATCH_CLOAD, P.PATCH_DLOAD, name), EXTRA)
    fr = T.dump(WORK, name, "U,RF")["steps"][0]["frames"][-1]
    return P.vec(fr, "U", NN)


def oracle(name, u, E=E0):
    """Return r = -RF with every DOF prescribed to u (loads kept)."""
    T.run_job(WORK, name, P.deck(P.patch_model_data(E), P.all_bcs(u), P.PATCH_CLOAD, P.PATCH_DLOAD, name), EXTRA)
    fr = T.dump(WORK, name, "U,RF")["steps"][0]["frames"][-1]
    return -P.vec(fr, "RF", NN)


def main():
    res = {"free_0based": FREE, "constrained_0based": CONS}
    u0 = solve("nom")
    r0 = oracle("or_nom", u0)
    res["r_free_at_nominal_maxabs"] = float(np.abs(r0[FREE]).max())

    # --- K from black-box oracle FD columns (linear problem: exact up to roundoff)
    eps = 1e-3
    Kfd = np.zeros((2 * NN, 2 * NN))
    for j in range(2 * NN):
        v = np.zeros(2 * NN)
        v[j] = eps
        rj = oracle("or_col%02d" % j, u0 + v)
        Kfd[:, j] = -(rj - r0) / eps
    Kan = P.cps4_K()
    res["K_oracleFD_vs_analytic_maxrel"] = float(np.abs(Kfd - Kan).max() / np.abs(Kan).max())
    print("K oracle-FD vs analytic max rel", res["K_oracleFD_vs_analytic_maxrel"])
    K = Kfd
    KFF = K[np.ix_(FREE, FREE)]
    KFC = K[np.ix_(FREE, CONS)]

    # --- analytic sensitivities (validation reference): K = E*K1
    K1 = Kan / E0
    F = P.patch_fext()
    K1FF = K1[np.ix_(FREE, FREE)]
    sE_an = np.zeros(2 * NN)
    sE_an[FREE] = -np.linalg.solve(K1FF, F[FREE]) / E0 ** 2
    sb_an = np.zeros(2 * NN)
    sb_an[IB] = 1.0
    sb_an[FREE] = -np.linalg.solve(K1FF, K1[np.ix_(FREE, [IB])].ravel())

    # --- E: pseudo-load from two frozen oracle runs
    out = {}
    for rel_h in (1e-2, 1e-4, 1e-6):
        h = rel_h * E0
        rh = oracle("or_E_%g" % rel_h, u0, E0 + h)
        p = (rh - r0) / h
        s = np.zeros(2 * NN)
        s[FREE] = np.linalg.solve(KFF, p[FREE])  # s_C = 0
        out["%g" % rel_h] = {"s_oracle": s.tolist(),
                             "rel_err_vs_analytic": float(np.abs(s - sE_an).max() / np.abs(sE_an).max())}
        print("E pseudo-load rel_h", rel_h, "rel err vs analytic", out["%g" % rel_h]["rel_err_vs_analytic"])
    res["E_oracle"] = out
    sE = np.array(out["%g" % 1e-4]["s_oracle"])  # pseudo-load step used downstream

    # --- full central-FD re-solves
    cfd = {}
    for rel_h in (1e-2, 1e-3, 1e-4):
        h = rel_h * E0
        up = solve("E_p_%g" % rel_h, E0 + h)
        um = solve("E_m_%g" % rel_h, E0 - h)
        sfd = (up - um) / (2 * h)
        cfd["%g" % rel_h] = {
            "rel_diff_oracle_vs_cfd": float(np.abs(sE - sfd).max() / np.abs(sfd).max()),
            "rel_diff_cfd_vs_analytic": float(np.abs(sfd - sE_an).max() / np.abs(sE_an).max())}
        print("E central FD rel_h", rel_h, cfd["%g" % rel_h])
    res["E_central_fd"] = cfd

    # --- BC parameter b: oracle with all DOFs at u* does not see b -> p = 0
    sb = np.zeros(2 * NN)
    sb[IB] = 1.0
    sb[FREE] = np.linalg.solve(KFF, -KFC.dot(sb[CONS]))
    hb = 1e-4
    sbfd = (solve("b_p", E0, B0 + hb) - solve("b_m", E0, B0 - hb)) / (2 * hb)
    res["b_rel_err_vs_analytic"] = float(np.abs(sb - sb_an).max() / np.abs(sb_an).max())
    res["b_rel_diff_vs_central_fd"] = float(np.abs(sb - sbfd).max() / np.abs(sbfd).max())
    res["b_wrong_if_sC_zero_rel"] = float(np.abs(0 * sb - sb_an).max() / np.abs(sb_an).max())
    print("b: oracle vs analytic", res["b_rel_err_vs_analytic"], "vs CFD", res["b_rel_diff_vs_central_fd"])

    # --- predictor consistency: r_F(u* + h s, E + h) ~ O(h^2); r_F(u*, E+h) ~ O(h)
    pc = []
    for rel_h in (1e-1, 3e-2, 1e-2, 3e-3, 1e-3):
        h = rel_h * E0
        rp = oracle("pc_pred_%g" % rel_h, u0 + h * sE, E0 + h)
        rz = oracle("pc_zero_%g" % rel_h, u0, E0 + h)
        pc.append({"rel_h": rel_h, "pred_resid_free_maxabs": float(np.abs(rp[FREE]).max()),
                   "zeroth_order_resid_free_maxabs": float(np.abs(rz[FREE]).max())})
        print("predictor", pc[-1])
    for k in range(1, len(pc)):
        a, b = pc[k - 1], pc[k]
        b["observed_order_pred"] = float(np.log(a["pred_resid_free_maxabs"] / b["pred_resid_free_maxabs"])
                                         / np.log(a["rel_h"] / b["rel_h"]))
        b["observed_order_zeroth"] = float(np.log(a["zeroth_order_resid_free_maxabs"] / b["zeroth_order_resid_free_maxabs"])
                                           / np.log(a["rel_h"] / b["rel_h"]))
    res["predictor"] = pc
    # analytic expectation for this linear-in-E model: r_F(u*+h s, E+h) = -h^2 K1_FF s_F
    res["predictor_expected_h2_coeff_maxabs"] = float(np.abs(K1FF.dot(sE[FREE])).max())
    res["sE_analytic"] = sE_an.tolist()
    res["sb_analytic"] = sb_an.tolist()
    T.write_json(os.path.join(RES, "e3_predictor.json"), res)


if __name__ == "__main__":
    main()
