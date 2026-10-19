# -*- coding: utf-8 -*-
"""E5: end-to-end first-order history sensitivity du_i/dE for J2 plasticity.

    abaqus python e5_plastic_gate.py

Model: 2x2 distorted CPS4 plate, J2 isotropic hardening, top edge displacement
control with load-unload-reload amplitude, CLOAD on a free and on a prescribed
DOF, *STATIC DIRECT 10 x 0.1 (tight equilibrium tolerance), output double.
Algorithm (spec section 3, forward difference in the parameter, all residual
evaluations by the frozen-displacement reaction oracle, r = -RF):
  for i = 1..N:
    r_nom  = r(u*_i ; nominal history_{i-1}, E)            [replay job]
    K_i    : columns  -[r(u*_i + eps e_j ; nominal hist) - r_nom]/eps, j free
    r_pert = r(u*_i ; COMMITTED perturbed history_{i-1}, E+h)  [replay job, E+h]
    p_i    = (r_pert - r_nom)/h
    K_FF s_F = p_F  (s_C = 0: BCs do not depend on E)
    commit u*_i + h s_i  (it is the last row of the next replays' history)
Reference: central FD of complete free re-solves at E(1 +- h_ref) on the same
DIRECT grid. Every history is reproduced by full replay from t = 0
(validated against restart in E2/E2d).
Writes results/e5_plastic_gate.json.
"""
from __future__ import print_function

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work", "e5")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]

S = 10.0
NODES = {}
for j in range(3):
    for i in range(3):
        NODES[j * 3 + i + 1] = (S * i / 2.0, S * j / 2.0)
NODES[5] = (5.5, 4.5)
ELEMS = {1: (1, 2, 5, 4), 2: (2, 3, 6, 5), 3: (4, 5, 8, 7), 4: (5, 6, 9, 8)}
NN = 9
E0, NU = 200000.0, 0.3
AMP = [(0.0, 0.0), (0.4, 1.0), (0.6, 0.3), (1.0, 1.2)]
UTOP = 0.15
BOTTOM, TOP = [1, 2, 3], [7, 8, 9]
CLOADS = [(7, 1, 100.0), (9, 2, -100.0)]
DT, NINC = 0.1, 10
TIMES = [DT * k for k in range(NINC + 1)]
CONTROLS = "*CONTROLS, PARAMETERS=FIELD, FIELD=DISPLACEMENT\n1e-10, 1e-08\n"
CONS = sorted([2 * (n - 1) + 1 for n in BOTTOM] + [0] + [2 * (n - 1) + 1 for n in TOP])
FREE = [k for k in range(2 * NN) if k not in CONS]
EPS_K = 1e-6       # displacement step for oracle tangent columns
H_REL = 1e-5       # parameter step for the pseudo-load
HREF_REL = (1e-3, 1e-4, 1e-5)
OUT = "*OUTPUT, FIELD, FREQUENCY=1\n*NODE OUTPUT\nU, RF\n*ELEMENT OUTPUT\nS, PEEQ\n"


def _arg(flag, default, conv):
    return conv(sys.argv[sys.argv.index(flag) + 1]) if flag in sys.argv else default


# Optional short studies (defaults reproduce the full gate):
#   --eps-k 1e-7 --central-k --nprobe 2 --tag epsK1e-7c
EPS_K = _arg("--eps-k", EPS_K, float)
H_REL = _arg("--h-rel", H_REL, float)
NPROBE = _arg("--nprobe", NINC, int)
CENTRAL_K = "--central-k" in sys.argv
TAG = _arg("--tag", "", str)
if TAG:
    WORK = os.path.join(HERE, "work", "e5_" + TAG)


def T_load(path):
    import json
    with open(path) as fh:
        return json.load(fh)


def fmt(x):
    return repr(float(x))


def amp(t):
    return float(np.interp(t, [a for a, b in AMP], [b for a, b in AMP]))


def model(E):
    s = "*NODE\n" + "".join("%d, %s, %s\n" % (k, fmt(v[0]), fmt(v[1])) for k, v in sorted(NODES.items()))
    s += "*ELEMENT, TYPE=CPS4, ELSET=PLATE\n" + "".join("%d, %d, %d, %d, %d\n" % ((e,) + c)
                                                       for e, c in sorted(ELEMS.items()))
    s += "*SOLID SECTION, ELSET=PLATE, MATERIAL=J2\n1.\n"
    s += "*MATERIAL, NAME=J2\n*ELASTIC\n%s, %s\n*PLASTIC\n250., 0.\n400., 0.1\n450., 0.5\n" % (fmt(E), fmt(NU))
    s += "*AMPLITUDE, NAME=AMP, DEFINITION=TABULAR\n" + ", ".join("%s, %s" % (fmt(a), fmt(b)) for a, b in AMP) + "\n"
    return s


def free_deck(E):
    s = "*HEADING\nE5 free\n" + model(E)
    s += "*STEP, INC=1000, EXTRAPOLATION=NO\n*STATIC, DIRECT\n%s, 1.\n" % fmt(DT) + CONTROLS
    s += "*BOUNDARY\n" + "".join("%d, 2, 2, 0.\n" % n for n in BOTTOM) + "1, 1, 1, 0.\n"
    s += "*BOUNDARY, AMPLITUDE=AMP\n" + "".join("%d, 2, 2, %s\n" % (n, fmt(UTOP)) for n in TOP)
    s += "*CLOAD, AMPLITUDE=AMP\n" + "".join("%d, %d, %s\n" % c for c in CLOADS)
    return s + OUT + "*END STEP\n"


def replay_deck(E, hist):
    """Every DOF prescribed along hist[0..m] (hist[0] = 0) via per-DOF tabular amplitudes."""
    m = len(hist)
    s = "*HEADING\nE5 replay\n" + model(E)
    for n in range(1, NN + 1):
        for d in (1, 2):
            s += "*AMPLITUDE, NAME=U%d_%d, DEFINITION=TABULAR\n" % (n, d)
            pairs = ["%s, %s" % (fmt(TIMES[k]), fmt(hist[k][2 * (n - 1) + d - 1])) for k in range(m)]
            for a in range(0, m, 4):
                s += ", ".join(pairs[a:a + 4]) + "\n"
    s += "*STEP, INC=1000\n*STATIC, DIRECT\n%s, %s\n" % (fmt(DT), fmt(TIMES[m - 1]))
    for n in range(1, NN + 1):
        for d in (1, 2):
            s += "*BOUNDARY, AMPLITUDE=U%d_%d\n%d, %d, %d, 1.\n" % (n, d, n, d, d)
    s += "*CLOAD, AMPLITUDE=AMP\n" + "".join("%d, %d, %s\n" % c for c in CLOADS)
    return s + OUT + "*END STEP\n"


def vec(frame, fname):
    v = np.zeros(2 * NN)
    for n, val in T.nodal(frame, fname, 2).items():
        v[2 * (n - 1):2 * (n - 1) + 2] = val
    return v


_count = [0]


def oracle(E, hist):
    _count[0] += 1
    name = "j%04d" % _count[0]
    T.run_job(WORK, name, replay_deck(E, hist), EXTRA)
    fr = T.dump(WORK, name, "RF")["steps"][0]["frames"][-1]
    for ext in (".odb", ".json"):  # keep the work folder small
        try:
            os.remove(os.path.join(WORK, name + ext))
        except OSError:
            pass
    return -vec(fr, "RF")


def free_run(name, E):
    T.run_job(WORK, name, free_deck(E), EXTRA)
    fr = T.dump(WORK, name, "U,PEEQ")["steps"][0]["frames"]
    U = [vec(f, "U") for f in fr]
    peeq = [max(v[4][0] for v in f["fields"]["PEEQ"]["values"]) if "PEEQ" in f["fields"] else 0.0 for f in fr]
    return U, peeq


def main():
    res = {"free_0based": FREE, "constrained_0based": CONS, "H_REL": H_REL, "EPS_K": EPS_K}
    Unom, peeq = free_run("nom", E0)
    res["max_peeq_per_frame"] = peeq
    h = H_REL * E0
    commits = [np.zeros(2 * NN)]
    S_or = [np.zeros(2 * NN)]
    rows = []
    for i in range(1, NPROBE + 1):
        hist_nom = Unom[:i]
        r_nom = oracle(E0, hist_nom + [Unom[i]])
        Kff = np.zeros((len(FREE), len(FREE)))
        for jj, j in enumerate(FREE):
            u = Unom[i].copy()
            u[j] += EPS_K
            if CENTRAL_K:
                um_ = Unom[i].copy()
                um_[j] -= EPS_K
                Kff[:, jj] = -(oracle(E0, hist_nom + [u])[FREE]
                               - oracle(E0, hist_nom + [um_])[FREE]) / (2 * EPS_K)
            else:
                Kff[:, jj] = -(oracle(E0, hist_nom + [u])[FREE] - r_nom[FREE]) / EPS_K
        r_pert = oracle(E0 + h, commits[:i] + [Unom[i]])
        p = (r_pert - r_nom) / h
        s = np.zeros(2 * NN)
        s[FREE] = np.linalg.solve(Kff, p[FREE])
        S_or.append(s)
        commits.append(Unom[i] + h * s)
        # predictor consistency at the committed state: r_F(u*+h s; committed hist, E+h)
        r_commit = oracle(E0 + h, commits[:i] + [commits[i]])
        rows.append({"inc": i, "r_nom_free_maxabs": float(np.abs(r_nom[FREE]).max()),
                     "p_free_maxabs": float(np.abs(p[FREE]).max()),
                     "K_sym_rel": float(np.abs(Kff - Kff.T).max() / np.abs(Kff).max()),
                     "commit_resid_free_maxabs": float(np.abs(r_commit[FREE]).max()),
                     "zeroth_resid_free_maxabs": float(np.abs(r_pert[FREE]).max())})
        print("inc", i, rows[-1], "jobs", _count[0])
    res["per_increment"] = rows
    res["s_oracle"] = [x.tolist() for x in S_or]
    if TAG:
        # short study: compare with the converged CFD reference stored by the full run
        ref = T_load(os.path.join(RES, "e5_plastic_gate.json"))["central_fd"]["0.0001"]["s_ref"]
        errs = []
        for i in range(1, NPROBE + 1):
            sr = np.array(ref[i])
            errs.append(float(np.abs(S_or[i][FREE] - sr[FREE]).max() / np.abs(sr[FREE]).max()))
        res["rel_err_vs_cfd_1e-4_per_inc"] = errs
        res["settings"] = {"EPS_K": EPS_K, "CENTRAL_K": CENTRAL_K, "H_REL": H_REL, "NPROBE": NPROBE}
        print(TAG, "rel err vs CFD per inc", ["%.2e" % e for e in errs])
        T.write_json(os.path.join(RES, "e5_plastic_gate_%s.json" % TAG), res)
        return
    # ---- references: central FD of complete re-solves on the same grid
    refs = {}
    for hr in HREF_REL:
        up, _ = free_run("p_%g" % hr, E0 * (1 + hr))
        um, _ = free_run("m_%g" % hr, E0 * (1 - hr))
        sref = [(a - b) / (2 * hr * E0) for a, b in zip(up, um)]
        errs = []
        for i in range(1, NINC + 1):
            den = np.abs(sref[i][FREE]).max()
            errs.append(float(np.abs(S_or[i][FREE] - sref[i][FREE]).max() / den))
        refs["%g" % hr] = {"rel_err_per_inc": errs, "max_rel_err": max(errs),
                           "s_ref": [x.tolist() for x in sref]}
        print("CFD h_rel", hr, "rel err per inc", ["%.2e" % e for e in errs])
    res["central_fd"] = refs
    res["n_oracle_jobs"] = _count[0]
    T.write_json(os.path.join(RES, "e5_plastic_gate.json"), res)


if __name__ == "__main__":
    main()
