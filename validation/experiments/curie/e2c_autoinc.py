# -*- coding: utf-8 -*-
"""E2c: automatic incrementation, time-schedule drift and step-per-increment replay.

    abaqus python e2c_autoinc.py

Same J2 plate as E2, but the free run uses AUTOMATIC incrementation.
  1. Free run at E and at E*(1+1e-3), E*(1+1e-2): do the accepted time grids match?
  2. Step-per-increment replay of the nominal automatic run (each step = one
     accepted increment with its actual dt, all DOFs prescribed): does it
     reproduce S/PEEQ at every accepted increment?
  3. Perturbed-branch forced onto the nominal grid: a FREE run at E(1+1e-3)
     driven by one *STATIC DIRECT step per nominal increment (same dt) -> it
     converges on exactly the nominal time grid.
Writes results/e2c_autoinc.json.
"""
from __future__ import print_function

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402
import e2_history as M  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work", "e2c")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]


def model_data(E):
    return M.model_data().replace("*ELASTIC\n%s," % M.fmt(M.E), "*ELASTIC\n%s," % M.fmt(E))


def auto_deck(E):
    s = "*HEADING\nE2c auto\n" + model_data(E)
    s += "*STEP, NAME=LOAD, INC=1000, EXTRAPOLATION=NO\n*STATIC\n0.07, 1., 1e-06, 0.13\n" + M.CONTROLS
    s += "*BOUNDARY\n" + "".join("%d, 2, 2, 0.\n" % n for n in M.BOTTOM) + "1, 1, 1, 0.\n"
    s += "*BOUNDARY, AMPLITUDE=AMP\n" + "".join("%d, 2, 2, %s\n" % (n, M.fmt(M.UTOP)) for n in M.TOP)
    s += "*CLOAD, AMPLITUDE=AMP\n" + "".join("%d, %d, %s\n" % c for c in M.CLOADS)
    s += M.OUT + "*END STEP\n"
    return s


def forced_grid_deck(E, times):
    """Free (not prescribed) perturbed run, one DIRECT step per nominal increment."""
    s = "*HEADING\nE2c forced grid\n" + model_data(E)
    for k in range(1, len(times)):
        dt = times[k] - times[k - 1]
        s += "*STEP, NAME=S%d, INC=10, EXTRAPOLATION=NO\n*STATIC, DIRECT\n%s, %s\n" % (k, M.fmt(dt), M.fmt(dt))
        s += M.CONTROLS
        if k == 1:
            s += "*BOUNDARY\n" + "".join("%d, 2, 2, 0.\n" % n for n in M.BOTTOM) + "1, 1, 1, 0.\n"
        s += "*BOUNDARY\n" + "".join("%d, 2, 2, %s\n" % (n, M.fmt(M.UTOP * M.amp(times[k]))) for n in M.TOP)
        s += "*CLOAD\n" + "".join("%d, %d, %s\n" % (n, d, M.fmt(v * M.amp(times[k]))) for n, d, v in M.CLOADS)
        s += M.OUT + "*END STEP\n"
    return s


def replay_step_deck(uhist, times):
    s = "*HEADING\nE2c replay step\n" + M.model_data()
    for k in range(1, len(uhist)):
        dt = times[k] - times[k - 1]
        s += "*STEP, NAME=S%d, INC=10\n*STATIC, DIRECT\n%s, %s\n" % (k, M.fmt(dt), M.fmt(dt))
        s += "*BOUNDARY\n" + "".join(
            "%d, %d, %d, %s\n" % (n, d, d, M.fmt(uhist[k][2 * (n - 1) + d - 1]))
            for n in range(1, M.NN + 1) for d in (1, 2))
        s += "*CLOAD\n" + "".join("%d, %d, %s\n" % (n, d, M.fmt(v * M.amp(times[k]))) for n, d, v in M.CLOADS)
        s += M.OUT + "*END STEP\n"
    return s


def main():
    res = {}
    grids = {}
    for tag, E in (("nom", M.E), ("p1e-3", M.E * 1.001), ("p1e-2", M.E * 1.01), ("p1e-1", M.E * 1.1)):
        T.run_job(WORK, "auto_" + tag, auto_deck(E), EXTRA)
        grids[tag] = [r["total_time"] for r in T.sta_increments(WORK, "auto_" + tag)
                      if not r["att"].endswith("U")]
        print(tag, len(grids[tag]), "increments")
    res["grids"] = grids
    res["grid_identical_p1e-3"] = grids["nom"] == grids["p1e-3"]
    res["grid_identical_p1e-2"] = grids["nom"] == grids["p1e-2"]
    res["grid_identical_p1e-1"] = grids["nom"] == grids["p1e-1"]
    print("grids identical:", res["grid_identical_p1e-3"], res["grid_identical_p1e-2"])

    d = T.dump(WORK, "auto_nom", "U,RF,S,PEEQ")
    ff = d["steps"][0]["frames"]
    times = [f["total_time"] for f in ff]
    U = [M.vec(f, "U") for f in ff]
    T.run_job(WORK, "replay_auto", replay_step_deck(U, times), EXTRA)
    dr = T.dump(WORK, "replay_auto", "U,RF,S,PEEQ")
    fr = [st["frames"][-1] for st in dr["steps"]]
    dS = [float(np.abs(M.ip(fr[k], "S") - M.ip(ff[k + 1], "S")).max()) for k in range(len(fr))]
    dP = [float(np.abs(M.ip(fr[k], "PEEQ") - M.ip(ff[k + 1], "PEEQ")).max()) for k in range(len(fr))]
    res["replay_auto_n_increments"] = len(fr)
    res["replay_auto_max_abs_dS"] = max(dS)
    res["replay_auto_max_abs_dPEEQ"] = max(dP)
    res["replay_auto_time_grid_max_abs_diff"] = float(max(abs(a - b["total_time"]) for a, b in zip(times[1:], fr)))
    print("replay of auto run: max dS", max(dS), "max dPEEQ", max(dP))

    T.run_job(WORK, "forced_p1e-3", forced_grid_deck(M.E * 1.001, times), EXTRA)
    dfg = T.dump(WORK, "forced_p1e-3", "U")
    tf = [st["frames"][-1]["total_time"] for st in dfg["steps"]]
    res["forced_grid_max_abs_time_diff"] = float(max(abs(a - b) for a, b in zip(times[1:], tf)))
    res["forced_grid_n_steps"] = len(tf)
    print("forced grid time diff", res["forced_grid_max_abs_time_diff"])
    T.write_json(os.path.join(RES, "e2c_autoinc.json"), res)


if __name__ == "__main__":
    main()
