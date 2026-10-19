# -*- coding: utf-8 -*-
"""E4: frozen-displacement oracle under NLGEOM with a follower pressure.

    abaqus python e4_nlgeom.py

CPS4 strip (8 x 2 elements, 40 x 4), clamped at x=0, follower pressure on the
top face of the whole strip (large deflection, rotations ~ tens of degrees).
Two materials:
  NEOH : *HYPERELASTIC, NEO HOOKE (path independent)
  HYPO : *ELASTIC (with NLGEOM Abaqus integrates a hypoelastic rate form,
         so the stress depends on the deformation PATH)
For each: nominal NLGEOM solve (fixed 10 increments) -> u*.
Oracle 1 ("one-shot"): every DOF prescribed to u* in ONE increment from the
  undeformed state, pressure kept -> RF_free should be ~0 if the oracle
  reproduces the nominal internal and follower external forces.
Oracle 2 ("replay"): every DOF prescribed along the accepted path (one step
  per increment) -> RF_free at the end.
Writes results/e4_nlgeom.json.
"""
from __future__ import print_function

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work", "e4")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]
NX, NY, LX, LY = 8, 2, 40.0, 4.0
NODES = {}
for j in range(NY + 1):
    for i in range(NX + 1):
        NODES[j * (NX + 1) + i + 1] = (LX * i / NX, LY * j / NY)
ELEMS = {}
for j in range(NY):
    for i in range(NX):
        n = lambda a, b: b * (NX + 1) + a + 1  # noqa: E731
        ELEMS[j * NX + i + 1] = (n(i, j), n(i + 1, j), n(i + 1, j + 1), n(i, j + 1))
NN = len(NODES)
CLAMP = [j * (NX + 1) + 1 for j in range(NY + 1)]
TOP_ELEMS = [(NY - 1) * NX + i + 1 for i in range(NX)]  # face S3 is the top edge
PRESS = 0.04  # linear-theory tip deflection ~12 (30% of length)
NINC = 10
MATS = {
    "NEOH": "*HYPERELASTIC, NEO HOOKE\n%s, %s\n" % (repr(200.0 / (4 * 1.3)), repr(6 * (1 - 0.6) / 200.0)),
    "HYPO": "*ELASTIC\n200., 0.3\n",
}


def fmt(x):
    return repr(float(x))


def model_data(mat):
    s = "*NODE\n" + "".join("%d, %s, %s\n" % (k, fmt(v[0]), fmt(v[1])) for k, v in sorted(NODES.items()))
    s += "*ELEMENT, TYPE=CPS4, ELSET=STRIP\n" + "".join(
        "%d, %d, %d, %d, %d\n" % ((e,) + c) for e, c in sorted(ELEMS.items()))
    s += "*SOLID SECTION, ELSET=STRIP, MATERIAL=M\n1.\n*MATERIAL, NAME=M\n" + MATS[mat]
    return s


OUT = "*OUTPUT, FIELD, FREQUENCY=1\n*NODE OUTPUT\nU, RF\n*ELEMENT OUTPUT\nS\n"


def dload():
    return "*DLOAD\n" + "".join("%d, P3, %s\n" % (e, fmt(PRESS)) for e in TOP_ELEMS)


def nominal_deck(mat):
    s = "*HEADING\nE4 nominal\n" + model_data(mat)
    s += "*STEP, NLGEOM=YES, INC=100\n*STATIC, DIRECT\n%s, 1.\n" % fmt(1.0 / NINC)
    s += "*CONTROLS, PARAMETERS=FIELD, FIELD=DISPLACEMENT\n1e-08, 1e-06\n"  # tight nominal equilibrium
    s += "*BOUNDARY\n" + "".join("%d, 1, 2, 0.\n" % n for n in CLAMP)
    s += dload() + OUT + "*END STEP\n"
    return s


def all_bc(u):
    return "*BOUNDARY\n" + "".join("%d, %d, %d, %s\n" % (n, d, d, fmt(u[2 * (n - 1) + d - 1]))
                                   for n in range(1, NN + 1) for d in (1, 2))


def oneshot_deck(mat, u):
    s = "*HEADING\nE4 oneshot oracle\n" + model_data(mat)
    s += "*STEP, NLGEOM=YES, INC=10\n*STATIC, DIRECT\n1., 1.\n" + all_bc(u) + dload() + OUT + "*END STEP\n"
    return s


def replay_deck(mat, uhist):
    s = "*HEADING\nE4 replay oracle\n" + model_data(mat)
    for k in range(1, len(uhist)):
        s += "*STEP, NLGEOM=YES, INC=10\n*STATIC, DIRECT\n%s, %s\n" % (fmt(1.0 / NINC), fmt(1.0 / NINC))
        s += all_bc(uhist[k])
        s += "*DLOAD\n" + "".join("%d, P3, %s\n" % (e, fmt(PRESS * k / NINC)) for e in TOP_ELEMS)
        s += OUT + "*END STEP\n"
    return s


def vec(frame, fname):
    d = T.nodal(frame, fname, 2)
    v = np.zeros(2 * NN)
    for n, val in d.items():
        v[2 * (n - 1):2 * (n - 1) + 2] = val
    return v


def main():
    cons = sorted(2 * (n - 1) + d for n in CLAMP for d in (0, 1))
    free = [k for k in range(2 * NN) if k not in cons]
    res = {}
    for mat in ("NEOH", "HYPO"):
        T.run_job(WORK, "nom_" + mat, nominal_deck(mat), EXTRA)
        fr = T.dump(WORK, "nom_" + mat, "U,RF,S")["steps"][0]["frames"]
        U = [vec(f, "U") for f in fr]
        rf_nom = vec(fr[-1], "RF")
        tip = U[-1][2 * (NX + 1) - 2:2 * (NX + 1)]
        # applied load scale: total pressure resultant
        scale = PRESS * LX
        T.run_job(WORK, "one_" + mat, oneshot_deck(mat, U[-1]), EXTRA)
        f1 = T.dump(WORK, "one_" + mat, "U,RF,S")["steps"][0]["frames"][-1]
        r1 = vec(f1, "RF")
        T.run_job(WORK, "rep_" + mat, replay_deck(mat, U), EXTRA)
        dr = T.dump(WORK, "rep_" + mat, "U,RF,S")
        f2 = dr["steps"][-1]["frames"][-1]
        r2 = vec(f2, "RF")
        S_nom = np.array([v[4] for v in fr[-1]["fields"]["S"]["values"]])
        S_one = np.array([v[4] for v in f1["fields"]["S"]["values"]])
        S_rep = np.array([v[4] for v in f2["fields"]["S"]["values"]])
        res[mat] = {
            "tip_displacement": tip.tolist(),
            "pressure_resultant_scale": scale,
            "oneshot_max_abs_RF_free": float(np.abs(r1[free]).max()),
            "oneshot_rel_RF_free": float(np.abs(r1[free]).max() / scale),
            "oneshot_max_abs_RF_cons_minus_nominal": float(np.abs(r1[cons] - rf_nom[cons]).max()),
            "oneshot_max_abs_S_minus_nominal": float(np.abs(S_one - S_nom).max()),
            "replay_max_abs_RF_free": float(np.abs(r2[free]).max()),
            "replay_rel_RF_free": float(np.abs(r2[free]).max() / scale),
            "replay_max_abs_S_minus_nominal": float(np.abs(S_rep - S_nom).max()),
            "nominal_max_abs_S": float(np.abs(S_nom).max()),
        }
        print(mat, res[mat])
    T.write_json(os.path.join(RES, "e4_nlgeom.json"), res)


if __name__ == "__main__":
    main()
