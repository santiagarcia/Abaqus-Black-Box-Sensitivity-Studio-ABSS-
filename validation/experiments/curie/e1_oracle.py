# -*- coding: utf-8 -*-
"""E1: frozen-displacement reaction oracle on T2D2 truss and CPS4 patch.

    abaqus python e1_oracle.py [--precision full|single]

For each model: (1) nominal solve; (2) u~ = u* + delta on ALL DOFs;
(3) oracle INP prescribing every DOF to u~ with the ORIGINAL loads kept;
(4) compare RF(u~) with analytic K u~ - F_ext (K from an independent numpy
element implementation used ONLY as validation reference, and cross-checked
against Abaqus *MATRIX GENERATE for the CPS4 patch).
Writes results/e1_<model>.json.
"""
from __future__ import print_function

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PREC = "full"
if "--precision" in sys.argv:
    PREC = sys.argv[sys.argv.index("--precision") + 1]
EXTRA = ["output_precision=" + PREC] if PREC == "full" else None
WORK = os.path.join(HERE, "work", "e1_" + PREC)
RES = os.path.join(HERE, "results")
if not os.path.isdir(RES):
    os.makedirs(RES)


def fmt(x):
    return repr(float(x))


# --------------------------------------------------------------------- truss
TRUSS_NODES = {1: (0.0, 0.0), 2: (2000.0, 0.0), 3: (1000.0, 1500.0)}
TRUSS_ELEMS = {1: (1, 3), 2: (2, 3), 3: (1, 2)}
TRUSS_E, TRUSS_A = 200000.0, 100.0
TRUSS_BC = [(1, 1, 0.0), (1, 2, 0.0), (2, 2, 0.0), (2, 1, 0.05)]
TRUSS_CLOAD = [(3, 1, 1000.0), (3, 2, -2000.0), (1, 2, 500.0), (2, 1, 300.0)]


def truss_model_data():
    s = "*NODE\n" + "".join("%d, %s, %s\n" % (n, fmt(x), fmt(y)) for n, (x, y) in sorted(TRUSS_NODES.items()))
    s += "*ELEMENT, TYPE=T2D2, ELSET=BARS\n" + "".join("%d, %d, %d\n" % (e, a, b) for e, (a, b) in sorted(TRUSS_ELEMS.items()))
    s += "*SOLID SECTION, ELSET=BARS, MATERIAL=STEEL\n%s\n" % fmt(TRUSS_A)
    s += "*MATERIAL, NAME=STEEL\n*ELASTIC\n%s, 0.3\n" % fmt(TRUSS_E)
    return s


def truss_K():
    K = np.zeros((6, 6))
    for e, (a, b) in TRUSS_ELEMS.items():
        xa, xb = np.array(TRUSS_NODES[a]), np.array(TRUSS_NODES[b])
        d = xb - xa
        L = np.linalg.norm(d)
        c = d / L
        k = TRUSS_E * TRUSS_A / L * np.outer(c, c)
        idx = [2 * (a - 1), 2 * (a - 1) + 1, 2 * (b - 1), 2 * (b - 1) + 1]
        ke = np.block([[k, -k], [-k, k]])
        K[np.ix_(idx, idx)] += ke
    return K


# ------------------------------------------------------------------ CPS4 patch
S = 10.0
PATCH_NODES = {1: (0, 0), 2: (1, 0), 3: (2, 0), 4: (0, 1), 5: (1.1, 0.9), 6: (2, 1),
               7: (0, 2), 8: (1, 2), 9: (2, 2)}
PATCH_NODES = dict((k, (S * v[0], S * v[1])) for k, v in PATCH_NODES.items())
PATCH_ELEMS = {1: (1, 2, 5, 4), 2: (2, 3, 6, 5), 3: (4, 5, 8, 7), 4: (5, 6, 9, 8)}
PATCH_E, PATCH_NU, PATCH_T = 200000.0, 0.3, 0.5
PATCH_BC = [(1, 1, 0.0), (1, 2, 0.0), (4, 1, 0.0), (7, 1, 0.0), (3, 2, 0.01)]
PATCH_CLOAD = [(9, 2, 50.0), (8, 1, 20.0), (4, 1, 30.0)]
PATCH_DLOAD = [(2, "P2", 100.0), (4, "P2", 100.0)]  # right edge x = 20


def patch_model_data(E=PATCH_E):
    s = "*NODE\n" + "".join("%d, %s, %s\n" % (n, fmt(x), fmt(y)) for n, (x, y) in sorted(PATCH_NODES.items()))
    s += "*ELEMENT, TYPE=CPS4, ELSET=PLATE\n" + "".join(
        "%d, %d, %d, %d, %d\n" % ((e,) + c) for e, c in sorted(PATCH_ELEMS.items()))
    s += "*SOLID SECTION, ELSET=PLATE, MATERIAL=MAT\n%s\n" % fmt(PATCH_T)
    s += "*MATERIAL, NAME=MAT\n*ELASTIC\n%s, %s\n" % (fmt(E), fmt(PATCH_NU))
    return s


def cps4_K(E=PATCH_E, nu=PATCH_NU, t=PATCH_T):
    D = E / (1 - nu ** 2) * np.array([[1, nu, 0], [nu, 1, 0], [0, 0, (1 - nu) / 2]])
    n = len(PATCH_NODES)
    K = np.zeros((2 * n, 2 * n))
    g = 1 / np.sqrt(3.0)
    xi_n = np.array([-1, 1, 1, -1.0])
    et_n = np.array([-1, -1, 1, 1.0])
    for e, conn in PATCH_ELEMS.items():
        X = np.array([PATCH_NODES[c] for c in conn])
        ke = np.zeros((8, 8))
        for xi in (-g, g):
            for et in (-g, g):
                dN = 0.25 * np.array([xi_n * (1 + et_n * et), et_n * (1 + xi_n * xi)])
                J = dN.dot(X)
                dNx = np.linalg.solve(J, dN)
                B = np.zeros((3, 8))
                B[0, 0::2] = dNx[0]
                B[1, 1::2] = dNx[1]
                B[2, 0::2] = dNx[1]
                B[2, 1::2] = dNx[0]
                ke += t * np.linalg.det(J) * B.T.dot(D).dot(B)
        idx = []
        for c in conn:
            idx += [2 * (c - 1), 2 * (c - 1) + 1]
        K[np.ix_(idx, idx)] += ke
    return K


def patch_fext(p_scale=1.0, t_factor=PATCH_T):
    n = len(PATCH_NODES)
    F = np.zeros(2 * n)
    for node, dof, v in PATCH_CLOAD:
        F[2 * (node - 1) + dof - 1] += v
    # pressure on face S2 (nodes 2-3 of the connectivity), outward normal +x
    for e, face, p in PATCH_DLOAD:
        conn = PATCH_ELEMS[e]
        a, b = conn[1], conn[2]
        xa, xb = np.array(PATCH_NODES[a]), np.array(PATCH_NODES[b])
        d = xb - xa
        L = np.linalg.norm(d)
        nrm = np.array([d[1], -d[0]]) / L  # outward for CCW element
        f = -p * p_scale * t_factor * L / 2.0 * nrm
        for c in (a, b):
            F[2 * (c - 1):2 * (c - 1) + 2] += f
    return F


def truss_fext():
    F = np.zeros(6)
    for node, dof, v in TRUSS_CLOAD:
        F[2 * (node - 1) + dof - 1] += v
    return F


# --------------------------------------------------------------------- decks
def deck(model_data, bcs, cloads, dloads, title):
    s = "*HEADING\n%s\n" % title + model_data
    s += "*STEP, NAME=LOAD\n*STATIC\n1., 1., 1e-05, 1.\n*BOUNDARY\n"
    s += "".join("%d, %d, %d, %s\n" % (n, d, d, fmt(v)) for n, d, v in bcs)
    if cloads:
        s += "*CLOAD\n" + "".join("%d, %d, %s\n" % (n, d, fmt(v)) for n, d, v in cloads)
    if dloads:
        s += "*DLOAD\n" + "".join("%d, %s, %s\n" % (e, f, fmt(v)) for e, f, v in dloads)
    s += "*OUTPUT, FIELD\n*NODE OUTPUT\nU, RF, CF\n*ELEMENT OUTPUT\nS, E\n*END STEP\n"
    return s


def vec(frame, fname, nnode):
    d = T.nodal(frame, fname, 2)
    v = np.zeros(2 * nnode)
    for n, val in d.items():
        v[2 * (n - 1):2 * (n - 1) + 2] = val
    return v


def all_bcs(u):
    n = len(u) // 2
    return [(i + 1, d + 1, u[2 * i + d]) for i in range(n) for d in range(2)]


def analyse(tag, model_data, nnode, bcs, cloads, dloads, K, Fext, delta):
    name_n = tag + "_nom"
    T.run_job(WORK, name_n, deck(model_data, bcs, cloads, dloads, tag + " nominal"), EXTRA)
    dn = T.dump(WORK, name_n, "U,RF,CF")
    fr = dn["steps"][0]["frames"][-1]
    u_star = vec(fr, "U", nnode)
    rf_star = vec(fr, "RF", nnode)
    cf_star = vec(fr, "CF", nnode) if "CF" in fr["fields"] else None
    prec = fr["fields"]["U"]["precision"]

    u_t = u_star + delta
    name_o = tag + "_oracle"
    T.run_job(WORK, name_o, deck(model_data, all_bcs(u_t), cloads, dloads, tag + " oracle"), EXTRA)
    do = T.dump(WORK, name_o, "U,RF,CF")
    fo = do["steps"][0]["frames"][-1]
    rf_o = vec(fo, "RF", nnode)
    u_o = vec(fo, "U", nnode)

    Ku = K.dot(u_t)
    hyp_plus = Ku - Fext          # RF = F_int - F_ext  (r = F_ext - F_int = -RF)
    hyp_excl = Ku - Fext_free_only(Fext, cloads, bcs, nnode)
    scale = np.max(np.abs(hyp_plus))
    loaded = sorted(set(2 * (n - 1) + d - 1 for n, d, v in cloads))
    out = {
        "precision": prec,
        "u_star": u_star.tolist(),
        "rf_star_nominal": rf_star.tolist(),
        "cf_nominal": None if cf_star is None else cf_star.tolist(),
        "delta": delta.tolist(),
        "u_tilde": u_t.tolist(),
        "u_read_back_maxabs_err": float(np.max(np.abs(u_o - u_t))),
        "rf_oracle": rf_o.tolist(),
        "K_u_minus_Fext": hyp_plus.tolist(),
        "max_abs_err_RF_vs_Ku_minus_Fext": float(np.max(np.abs(rf_o - hyp_plus))),
        "max_rel_err_RF_vs_Ku_minus_Fext": float(np.max(np.abs(rf_o - hyp_plus)) / scale),
        "max_abs_err_RF_vs_Ku_minus_Fext_loaded_constrained_excluded": float(np.max(np.abs(rf_o - hyp_excl))),
        "max_abs_err_RF_vs_Ku_only": float(np.max(np.abs(rf_o - Ku))),
        "cload_dofs_0based": loaded,
        "nominal_free_dof_imbalance_check": None,
    }
    # nominal check: RF* at constrained DOFs vs K u* - F_ext
    r_nom = K.dot(u_star) - Fext
    cons = sorted(set(2 * (n - 1) + d - 1 for n, d, v in bcs))
    free = [i for i in range(2 * nnode) if i not in cons]
    out["nominal_constrained_RF_minus_(Ku-F)_maxabs"] = float(np.max(np.abs(rf_star[cons] - r_nom[cons])))
    out["nominal_free_dof_imbalance_check"] = float(np.max(np.abs(r_nom[free])))
    out["free_dofs_0based"] = free
    out["constrained_dofs_0based"] = cons
    print(tag, "precision", prec)
    print(tag, "max |RF - (K u~ - F)| =", out["max_abs_err_RF_vs_Ku_minus_Fext"], "rel", out["max_rel_err_RF_vs_Ku_minus_Fext"])
    print(tag, "max |RF - K u~| =", out["max_abs_err_RF_vs_Ku_only"])
    print(tag, "max |RF - (K u~ - F_free_only)| =", out["max_abs_err_RF_vs_Ku_minus_Fext_loaded_constrained_excluded"])
    print(tag, "nominal: constrained RF vs Ku-F", out["nominal_constrained_RF_minus_(Ku-F)_maxabs"],
          "free imbalance", out["nominal_free_dof_imbalance_check"])
    return out


def Fext_free_only(Fext, cloads, bcs, nnode):
    """F_ext with CLOADs on originally constrained DOFs removed (alternative hypothesis)."""
    F = Fext.copy()
    cons = set(2 * (n - 1) + d - 1 for n, d, v in bcs)
    for n, d, v in cloads:
        i = 2 * (n - 1) + d - 1
        if i in cons:
            F[i] -= v
    return F


def matrix_generate_patch():
    """Cross-check analytic CPS4 K with Abaqus *MATRIX GENERATE."""
    name = "patch_mtx"
    s = "*HEADING\npatch matrix generate\n" + patch_model_data()
    # FORMAT=MATRIX INPUT writes "node, dof, node, dof, value" (explicit DOF
    # labels); FORMAT=COORDINATE writes "eq, eq, value" with internal
    # equation numbers and no DOF map, so it is NOT safe on its own.
    s += "*STEP, NAME=MTX\n*MATRIX GENERATE, STIFFNESS\n*MATRIX OUTPUT, STIFFNESS, FORMAT=MATRIX INPUT\n*END STEP\n"
    T.run_job(WORK, name, s, EXTRA)
    path = None
    for f in os.listdir(WORK):
        if f.upper().startswith("PATCH_MTX_STIF") and f.lower().endswith(".mtx"):
            path = os.path.join(WORK, f)
    n = len(PATCH_NODES)
    K = np.zeros((2 * n, 2 * n))
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("*"):
                continue
            p = [x for x in line.replace(",", " ").split()]
            if len(p) == 5:
                n1, d1, n2, d2, v = int(p[0]), int(p[1]), int(p[2]), int(p[3]), float(p[4])
                i, j = 2 * (n1 - 1) + d1 - 1, 2 * (n2 - 1) + d2 - 1
                K[i, j] = v
                K[j, i] = v
    return K, path


def main():
    results = {"precision_option": PREC}
    # ---------------- truss
    Kt = truss_K()
    Ft = truss_fext()
    delta_t = np.array([0.011, -0.007, 0.013, 0.005, -0.017, 0.019])
    results["truss"] = analyse("truss", truss_model_data(), 3, TRUSS_BC, TRUSS_CLOAD, [], Kt, Ft, delta_t)
    # ---------------- patch
    Kp = cps4_K()
    Kmtx, mpath = matrix_generate_patch()
    rel = float(np.max(np.abs(Kp - Kmtx)) / np.max(np.abs(Kp)))
    print("CPS4 analytic K vs *MATRIX GENERATE: max rel diff", rel)
    results["patch_K_analytic_vs_matrix_generate_maxrel"] = rel
    results["patch_mtx_file"] = os.path.basename(mpath)
    Fp = patch_fext()
    rng = np.random.RandomState(7)
    delta_p = 1e-3 * rng.uniform(-1, 1, 18)
    results["patch"] = analyse("patch", patch_model_data(), 9, PATCH_BC, PATCH_CLOAD, PATCH_DLOAD, Kp, Fp, delta_p)
    # thickness hypothesis for pressure: compare with F_ext built without thickness factor
    Fp_not = patch_fext(t_factor=1.0)
    rf = np.array(results["patch"]["rf_oracle"])
    ut = np.array(results["patch"]["u_tilde"])
    results["patch"]["max_abs_err_if_pressure_NOT_times_thickness"] = float(
        np.max(np.abs(rf - (Kp.dot(ut) - Fp_not))))
    T.write_json(os.path.join(RES, "e1_%s.json" % PREC), results)


if __name__ == "__main__":
    main()
