# -*- coding: utf-8 -*-
"""E2e: is *MATRIX GENERATE after a converged plastic increment the probe tangent?

    abaqus python e2e_matrix_tangent.py   (requires results/e2_history.json and work/e2 from E2)

Step-per-increment replay of the nominal J2 history; after the general steps
for increments 6, 13 (elastic) and 18 a *MATRIX GENERATE, STIFFNESS
perturbation step is inserted. The matrix is compared with
  (a) the analytic elastic K (validation reference), and
  (b) the directional derivative of the frozen probe measured in E2:
      K_probe delta ~= 10 * [RF(u*_i + delta/10) - RF(u*_i)]   (history i-1 fixed).
Writes results/e2e_matrix_tangent.json.
"""
from __future__ import print_function

import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402
import e2_history as M  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work", "e2e")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]
AT = [6, 13, 18]


def read_mtx(path, nn):
    K = np.zeros((2 * nn, 2 * nn))
    with open(path) as fh:
        for line in fh:
            p = line.replace(",", " ").split()
            if len(p) == 5 and not line.startswith("*"):
                i = 2 * (int(p[0]) - 1) + int(p[1]) - 1
                j = 2 * (int(p[2]) - 1) + int(p[3]) - 1
                K[i, j] = float(p[4])
                K[j, i] = float(p[4])
    return K


def main():
    e2 = json.load(open(os.path.join(RES, "e2_history.json")))
    fd = T.dump(os.path.join(HERE, "work", "e2"), "free", "U")
    U = [M.vec(f, "U") for f in fd["steps"][0]["frames"]]
    s = "*HEADING\nE2e matrix tangent\n" + M.model_data()
    stepno = 0
    stepmap = {}
    for k in range(1, max(AT) + 1):
        s += "*STEP, NAME=S%d, INC=10\n*STATIC, DIRECT\n%s, %s\n" % (k, M.fmt(M.DT), M.fmt(M.DT))
        s += "*BOUNDARY\n" + "".join("%d, %d, %d, %s\n" % (n, d, d, M.fmt(U[k][2 * (n - 1) + d - 1]))
                                     for n in range(1, M.NN + 1) for d in (1, 2))
        s += "*END STEP\n"
        stepno += 1
        if k in AT:
            s += "*STEP, NAME=MTX%d\n*MATRIX GENERATE, STIFFNESS\n" % k
            # base-state BCs put 1e36 on constrained diagonals; release them in the perturbation step
            s += "*BOUNDARY, OP=NEW\n"
            s += "*MATRIX OUTPUT, STIFFNESS, FORMAT=MATRIX INPUT\n*END STEP\n"
            stepno += 1
            stepmap[k] = stepno
    T.run_job(WORK, "mtx", s, EXTRA)
    nodes_ref = dict((k, tuple(v)) for k, v in M.NODES.items())
    Kel = T.cps4_stiffness(nodes_ref, M.ELEMS, M.E, M.NU, M.THK)
    delta = np.array(e2["delta"])
    out = {"step_numbers": stepmap}
    files = os.listdir(WORK)
    for k in AT:
        cand = [f for f in files if f.upper() == ("MTX_STIF%d.MTX" % stepmap[k])]
        Km = read_mtx(os.path.join(WORK, cand[0]), M.NN)
        # probe directional derivative from E2 (replay probe, delta and delta/10 not stored
        # separately for d2 -> recompute from linearity metric is not possible; use d1-d0)
        pr = e2["probes"][str(k)]
        dRF = np.array(pr["d1"]["RF_replay"]) - np.array(pr["d0"]["RF_replay"])
        Kd = Km.dot(delta)
        out[str(k)] = {
            "file": cand[0],
            "Kmtx_vs_Kel_maxrel": float(np.abs(Km - Kel).max() / np.abs(Kel).max()),
            "Kmtx_delta_vs_probe_dRF_rel": float(np.abs(Kd - dRF).max() / np.abs(dRF).max()),
            "Kel_delta_vs_probe_dRF_rel": float(np.abs(Kel.dot(delta) - dRF).max() / np.abs(dRF).max()),
            "probe_linearity_rel_diff": pr.get("linearity_rel_diff_delta_vs_delta_over_10"),
        }
        print(k, out[str(k)])
    T.write_json(os.path.join(RES, "e2e_matrix_tangent.json"), out)


if __name__ == "__main__":
    main()
