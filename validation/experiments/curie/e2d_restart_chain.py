# -*- coding: utf-8 -*-
"""E2d: chained restarts (commit-branch pattern) reproduce the J2 history.

    abaqus python e2d_restart_chain.py      (requires work/e2/free.* from e2_history.py)

Commit-branch pattern of the sensitivity algorithm: each increment k is a new
restart job that reads the previous job's restart file and prescribes every
DOF to a target displacement (here the nominal U_k, so the exact answer is
the free run's state), writing its own restart for the next link.
  link 6: oldjob=free,    *RESTART, READ, STEP=1, INC=5, END STEP
  link 7: oldjob=chain06, *RESTART, READ, STEP=2, INC=1
  link 8: oldjob=chain07, *RESTART, READ, STEP=3, INC=1
Compares S/PEEQ/RF with the free run at increments 6, 7, 8.
Writes results/e2d_restart_chain.json.
"""
from __future__ import print_function

import os
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402
import e2_history as M  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "work", "e2")
WORK = os.path.join(HERE, "work", "e2d")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]


def link_deck(read_card, k, u):
    s = "*HEADING\nE2d chain link %d\n%s\n" % (k, read_card)
    s += "*STEP, NAME=C%d, INC=10\n*STATIC, DIRECT\n%s, %s\n" % (k, M.fmt(M.DT), M.fmt(M.DT))
    s += "*RESTART, WRITE, FREQUENCY=1\n"
    s += "*BOUNDARY\n" + "".join("%d, %d, %d, %s\n" % (n, d, d, M.fmt(u[2 * (n - 1) + d - 1]))
                                 for n in range(1, M.NN + 1) for d in (1, 2))
    s += "*CLOAD\n" + "".join("%d, %d, %s\n" % (n, d, M.fmt(v * M.amp(M.TIMES[k]))) for n, d, v in M.CLOADS)
    s += M.OUT + "*END STEP\n"
    return s


def main():
    if not os.path.isdir(WORK):
        os.makedirs(WORK)
    for ext in (".res", ".mdl", ".stt", ".prt", ".odb", ".sim", ".inp"):  # .sim is REQUIRED (pre.exe crashes without it)
        shutil.copy(os.path.join(SRC, "free" + ext), os.path.join(WORK, "free" + ext))
    fd = T.dump(WORK, "free", "U,RF,S,PEEQ")
    ff = fd["steps"][0]["frames"]
    U = [M.vec(f, "U") for f in ff]
    out = {}
    prev, card = "free", "*RESTART, READ, STEP=1, INC=5, END STEP"
    for idx, k in enumerate((6, 7, 8)):
        name = "chain%02d" % k
        T.run_job(WORK, name, link_deck(card, k, U[k]), EXTRA + ["oldjob=" + prev])
        d = T.dump(WORK, name, "U,RF,S,PEEQ")
        fr = d["steps"][-1]["frames"][-1]
        out[str(k)] = {
            "total_time": fr["total_time"],
            "max_abs_dS": float(np.abs(M.ip(fr, "S") - M.ip(ff[k], "S")).max()),
            "max_abs_dPEEQ": float(np.abs(M.ip(fr, "PEEQ") - M.ip(ff[k], "PEEQ")).max()),
            "steps_in_odb": [s["name"] for s in d["steps"]],
        }
        print(k, out[str(k)])
        prev = name
        card = "*RESTART, READ, STEP=%d, INC=1" % (idx + 2)
    T.write_json(os.path.join(RES, "e2d_restart_chain.json"), out)


if __name__ == "__main__":
    main()
