# -*- coding: utf-8 -*-
"""E2: history reproduction for frozen-displacement probes in J2 plasticity.

    abaqus python e2_history.py

Model: 3x3 distorted CPS4 plate, J2 (*PLASTIC, isotropic), top edge
displacement-controlled with load-unload-reload amplitude, CLOAD on a free
DOF and on a prescribed DOF, *STATIC DIRECT fixed increments (20 x 0.05).

Compared mechanisms for "state at i-1 from the free run, current u prescribed":
  (A) replay-amp  : one step, every DOF prescribed via its own TABULAR amplitude
                    through the accepted displacement history (DIRECT, same dt)
  (B) replay-step : one *STATIC DIRECT step per accepted increment, every DOF
                    prescribed to the accepted displacement of that increment
  (C) restart     : *RESTART, READ, STEP=1, INC=i-1, END STEP + new one-
                    increment step prescribing every DOF.
Writes results/e2_history.json.
"""
from __future__ import print_function

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import abqtools as T  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work", "e2")
RES = os.path.join(HERE, "results")
EXTRA = ["output_precision=full"]

H = 10.0 / 3.0
NODES = {}
for j in range(4):
    for i in range(4):
        NODES[j * 4 + i + 1] = [i * H, j * H]
for (i, j), (dx, dy) in {(1, 1): (0.4, 0.3), (2, 1): (-0.3, 0.2), (1, 2): (0.2, -0.4),
                         (2, 2): (-0.25, -0.3)}.items():
    NODES[j * 4 + i + 1][0] += dx
    NODES[j * 4 + i + 1][1] += dy
ELEMS = {}
for j in range(3):
    for i in range(3):
        n = lambda a, b: b * 4 + a + 1  # noqa: E731
        ELEMS[j * 3 + i + 1] = (n(i, j), n(i + 1, j), n(i + 1, j + 1), n(i, j + 1))
NN = len(NODES)
E, NU, THK = 200000.0, 0.3, 1.0
AMP = [(0.0, 0.0), (0.4, 1.0), (0.6, 0.3), (1.0, 1.2)]
UTOP = 0.15
BOTTOM = [1, 2, 3, 4]
TOP = [13, 14, 15, 16]
CLOADS = [(13, 1, 100.0), (16, 2, -100.0)]
DT, NINC = 0.05, 20
TIMES = [DT * k for k in range(NINC + 1)]
PROBES = [6, 11, 13, 18]  # 13 is an all-elastic reloading increment
CONTROLS = "*CONTROLS, PARAMETERS=FIELD, FIELD=DISPLACEMENT\n1e-08, 1e-06\n"


def fmt(x):
    return repr(float(x))


def amp(t):
    ts = [a for a, b in AMP]
    vs = [b for a, b in AMP]
    return float(np.interp(t, ts, vs))


def model_data():
    s = "*NODE\n" + "".join("%d, %s, %s\n" % (k, fmt(v[0]), fmt(v[1])) for k, v in sorted(NODES.items()))
    s += "*ELEMENT, TYPE=CPS4, ELSET=PLATE\n" + "".join(
        "%d, %d, %d, %d, %d\n" % ((e,) + c) for e, c in sorted(ELEMS.items()))
    s += "*SOLID SECTION, ELSET=PLATE, MATERIAL=J2\n%s\n" % fmt(THK)
    s += "*MATERIAL, NAME=J2\n*ELASTIC\n%s, %s\n*PLASTIC\n250., 0.\n400., 0.1\n450., 0.5\n" % (fmt(E), fmt(NU))
    s += "*AMPLITUDE, NAME=AMP, DEFINITION=TABULAR\n" + ", ".join("%s, %s" % (fmt(a), fmt(b)) for a, b in AMP) + "\n"
    return s


OUT = "*OUTPUT, FIELD, FREQUENCY=1\n*NODE OUTPUT\nU, RF, CF\n*ELEMENT OUTPUT\nS, PEEQ, PE\n"


def free_deck():
    s = "*HEADING\nE2 free J2 run\n" + model_data()
    s += "*STEP, NAME=LOAD, INC=1000, EXTRAPOLATION=NO\n*STATIC, DIRECT\n%s, 1.\n" % fmt(DT)
    s += CONTROLS
    s += "*RESTART, WRITE, FREQUENCY=1\n"
    s += "*BOUNDARY\n" + "".join("%d, 2, 2, 0.\n" % n for n in BOTTOM) + "1, 1, 1, 0.\n"
    s += "*BOUNDARY, AMPLITUDE=AMP\n" + "".join("%d, 2, 2, %s\n" % (n, fmt(UTOP)) for n in TOP)
    s += "*CLOAD, AMPLITUDE=AMP\n" + "".join("%d, %d, %s\n" % c for c in CLOADS)
    s += OUT + "*END STEP\n"
    return s


def replay_amp_deck(uhist, t_end, title):
    """uhist: list of (2*NN) arrays at TIMES[0..m]; prescribes every DOF."""
    m = len(uhist)
    s = "*HEADING\n%s\n" % title + model_data()
    for n in range(1, NN + 1):
        for d in (1, 2):
            s += "*AMPLITUDE, NAME=U%d_%d, DEFINITION=TABULAR\n" % (n, d)
            pairs = ["%s, %s" % (fmt(TIMES[k]), fmt(uhist[k][2 * (n - 1) + d - 1])) for k in range(m)]
            for a in range(0, len(pairs), 4):
                s += ", ".join(pairs[a:a + 4]) + "\n"
    s += "*STEP, NAME=REPLAY, INC=1000\n*STATIC, DIRECT\n%s, %s\n" % (fmt(DT), fmt(t_end))
    for n in range(1, NN + 1):
        for d in (1, 2):
            s += "*BOUNDARY, AMPLITUDE=U%d_%d\n%d, %d, %d, 1.\n" % (n, d, n, d, d)
    s += "*CLOAD, AMPLITUDE=AMP\n" + "".join("%d, %d, %s\n" % c for c in CLOADS)
    s += OUT + "*END STEP\n"
    return s


def replay_step_deck(uhist, title):
    s = "*HEADING\n%s\n" % title + model_data()
    for k in range(1, len(uhist)):
        dt = TIMES[k] - TIMES[k - 1]
        s += "*STEP, NAME=S%d, INC=10\n*STATIC, DIRECT\n%s, %s\n" % (k, fmt(dt), fmt(dt))
        s += "*BOUNDARY\n" + "".join(
            "%d, %d, %d, %s\n" % (n, d, d, fmt(uhist[k][2 * (n - 1) + d - 1]))
            for n in range(1, NN + 1) for d in (1, 2))
        s += "*CLOAD\n" + "".join("%d, %d, %s\n" % (n, d, fmt(v * amp(TIMES[k]))) for n, d, v in CLOADS)
        s += OUT + "*END STEP\n"
    return s


def restart_deck(i, u_target, title):
    s = "*HEADING\n%s\n*RESTART, READ, STEP=1, INC=%d, END STEP\n" % (title, i - 1)
    s += "*STEP, NAME=PROBE, INC=10\n*STATIC, DIRECT\n%s, %s\n" % (fmt(DT), fmt(DT))
    s += "*BOUNDARY\n" + "".join(
        "%d, %d, %d, %s\n" % (n, d, d, fmt(u_target[2 * (n - 1) + d - 1]))
        for n in range(1, NN + 1) for d in (1, 2))
    s += "*CLOAD\n" + "".join("%d, %d, %s\n" % (n, d, fmt(v * amp(TIMES[i]))) for n, d, v in CLOADS)
    s += OUT + "*END STEP\n"
    return s


def vec(frame, fname):
    d = T.nodal(frame, fname, 2)
    v = np.zeros(2 * NN)
    for n, val in d.items():
        v[2 * (n - 1):2 * (n - 1) + 2] = val
    return v


def ip(frame, fname):
    d = T.ipfield(frame, fname)
    keys = sorted(d)
    return np.array([d[k] for k in keys]).reshape(len(keys), -1)


def frames_of(dump):
    out = []
    for st in dump["steps"]:
        out.extend(st["frames"] if len(dump["steps"]) == 1 else st["frames"][-1:])
    return out


def main():
    res = {}
    cons_orig = sorted([2 * (n - 1) + 1 for n in BOTTOM] + [0] + [2 * (n - 1) + 1 for n in TOP])
    free_orig = [k for k in range(2 * NN) if k not in cons_orig]
    loaded = [2 * (n - 1) + d - 1 for n, d, v in CLOADS]
    res["dofs"] = {"constrained_0based": cons_orig, "free_0based": free_orig, "cload_0based": loaded}

    # ---------------- free run
    T.run_job(WORK, "free", free_deck(), EXTRA)
    sta = T.sta_increments(WORK, "free")
    res["free_sta"] = sta
    fd = T.dump(WORK, "free", "U,RF,S,PEEQ")
    ff = fd["steps"][0]["frames"]
    assert len(ff) == NINC + 1, len(ff)
    U = [vec(f, "U") for f in ff]
    RF = [vec(f, "RF") for f in ff]
    Sf = [ip(f, "S") for f in ff[1:]]
    Pf = [ip(f, "PEEQ") for f in ff[1:]]
    res["free_times"] = [f["total_time"] for f in ff]
    res["free_max_peeq_per_inc"] = [float(p.max()) for p in Pf]
    res["free_max_abs_S_per_inc"] = [float(np.abs(s).max()) for s in Sf]
    dPEEQ_inc = [float(np.abs(Pf[0]).max())] + [float(np.abs(Pf[k] - Pf[k - 1]).max()) for k in range(1, NINC)]
    res["free_max_dPEEQ_per_inc"] = dPEEQ_inc

    def compare(frames, label):
        dS = [float(np.abs(ip(frames[k + 1], "S") - Sf[k]).max()) for k in range(NINC)]
        dP = [float(np.abs(ip(frames[k + 1], "PEEQ") - Pf[k]).max()) for k in range(NINC)]
        rf = [vec(frames[k], "RF") for k in range(1, NINC + 1)]
        rfree = [float(np.abs(r[free_orig]).max()) for r in rf]
        rcons = [float(np.abs(rf[k - 1][cons_orig] - RF[k][cons_orig]).max()) for k in range(1, NINC + 1)]
        times = [f["total_time"] for f in frames]
        out = {"max_abs_dS_per_inc": dS, "max_abs_dPEEQ_per_inc": dP,
               "max_abs_RF_on_originally_free_dofs_per_inc": rfree,
               "max_abs_RF_diff_on_originally_constrained_per_inc": rcons,
               "times": times,
               "max_abs_dS": max(dS), "max_abs_dPEEQ": max(dP),
               "max_rel_dS": max(dS) / max(res["free_max_abs_S_per_inc"]),
               "max_free_residual": max(rfree)}
        print(label, "max|dS|", max(dS), "max|dPEEQ|", max(dP), "max|RF_free|", max(rfree),
              "max|dRF_cons|", max(rcons))
        return out

    # ---------------- (A) replay with per-DOF amplitudes
    T.run_job(WORK, "replay_amp", replay_amp_deck(U, 1.0, "E2 replay amp"), EXTRA)
    da = T.dump(WORK, "replay_amp", "U,RF,S,PEEQ")
    fa = da["steps"][0]["frames"]
    res["replay_amp"] = compare(fa, "replay_amp")
    res["replay_amp"]["n_frames"] = len(fa)

    # ---------------- (B) replay one step per increment
    T.run_job(WORK, "replay_step", replay_step_deck(U, "E2 replay step"), EXTRA)
    db = T.dump(WORK, "replay_step", "U,RF,S,PEEQ")
    fb = [db["steps"][0]["frames"][0]] + [st["frames"][-1] for st in db["steps"]]
    res["replay_step"] = compare(fb, "replay_step")

    # ---------------- probes
    rng = np.random.RandomState(11)
    delta = 1e-4 * rng.uniform(-1, 1, 2 * NN)
    nodes_ref = dict((k, tuple(v)) for k, v in NODES.items())
    Kel = T.cps4_stiffness(nodes_ref, ELEMS, E, NU, THK)
    res["probes"] = {}
    for i in PROBES:
        pr = {}
        for tag, dvec in (("d0", np.zeros(2 * NN)), ("d1", delta)):
            ut = U[i] + dvec
            hist = U[:i] + [ut]
            na = "pa_%d_%s" % (i, tag)
            T.run_job(WORK, na, replay_amp_deck(hist, TIMES[i], "probe replay"), EXTRA)
            fa_ = T.dump(WORK, na, "U,RF,S,PEEQ")["steps"][0]["frames"][-1]
            nr = "pr_%d_%s" % (i, tag)
            T.run_job(WORK, nr, restart_deck(i, ut, "probe restart"), EXTRA + ["oldjob=free"])
            drs = T.dump(WORK, nr, "U,RF,S,PEEQ")
            fr_ = drs["steps"][-1]["frames"][-1]
            ra, rr = vec(fa_, "RF"), vec(fr_, "RF")
            pr[tag] = {
                "replay_time": fa_["total_time"], "restart_time": fr_["total_time"],
                "restart_n_steps_in_odb": len(drs["steps"]),
                "u_readback_err_replay": float(np.abs(vec(fa_, "U") - ut).max()),
                "u_readback_err_restart": float(np.abs(vec(fr_, "U") - ut).max()),
                "RF_replay": ra.tolist(), "RF_restart": rr.tolist(),
                "max_abs_RF_replay_minus_restart": float(np.abs(ra - rr).max()),
                "max_abs_RF": float(np.abs(ra).max()),
                "max_abs_S_replay_minus_restart": float(np.abs(ip(fa_, "S") - ip(fr_, "S")).max()),
                "max_abs_PEEQ_replay_minus_restart": float(np.abs(ip(fa_, "PEEQ") - ip(fr_, "PEEQ")).max()),
                "max_abs_RF_free_dofs_replay": float(np.abs(ra[free_orig]).max()),
                "max_abs_RF_free_dofs_restart": float(np.abs(rr[free_orig]).max()),
            }
            if tag == "d0":
                pr[tag]["max_abs_S_replay_vs_free"] = float(np.abs(ip(fa_, "S") - Sf[i - 1]).max())
                pr[tag]["max_abs_S_restart_vs_free"] = float(np.abs(ip(fr_, "S") - Sf[i - 1]).max())
                pr[tag]["max_abs_PEEQ_restart_vs_free"] = float(np.abs(ip(fr_, "PEEQ") - Pf[i - 1]).max())
                pr[tag]["max_abs_RF_cons_restart_vs_free"] = float(np.abs(rr[cons_orig] - RF[i][cons_orig]).max())
            print("probe", i, tag, "RF replay-restart", pr[tag]["max_abs_RF_replay_minus_restart"],
                  "|RF|", pr[tag]["max_abs_RF"], "S diff", pr[tag]["max_abs_S_replay_minus_restart"])
        pr["increment_is_elastic_everywhere"] = bool(dPEEQ_inc[i - 1] == 0.0)
        for meth in ("replay", "restart"):
            dRF = np.array(pr["d1"]["RF_" + meth]) - np.array(pr["d0"]["RF_" + meth])
            Kd = Kel.dot(delta)
            pr["dRF_vs_Kel_delta_maxabs_" + meth] = float(np.abs(dRF - Kd).max())
            pr["dRF_vs_Kel_delta_rel_" + meth] = float(np.abs(dRF - Kd).max() / np.abs(Kd).max())
        print("probe", i, "elastic", pr["increment_is_elastic_everywhere"],
              "dRF vs K_el delta rel (replay, restart)",
              pr["dRF_vs_Kel_delta_rel_replay"], pr["dRF_vs_Kel_delta_rel_restart"])
        # linearity of the probe map: delta vs delta/10 (replay only)
        ut = U[i] + 0.1 * delta
        na = "pa_%d_d2" % i
        T.run_job(WORK, na, replay_amp_deck(U[:i] + [ut], TIMES[i], "probe replay"), EXTRA)
        r2 = vec(T.dump(WORK, na, "RF")["steps"][0]["frames"][-1], "RF")
        r0 = np.array(pr["d0"]["RF_replay"])
        r1 = np.array(pr["d1"]["RF_replay"])
        a1, a2 = r1 - r0, 10.0 * (r2 - r0)
        pr["linearity_rel_diff_delta_vs_delta_over_10"] = float(np.abs(a1 - a2).max() / np.abs(a2).max())
        print("probe", i, "linearity rel diff", pr["linearity_rel_diff_delta_vs_delta_over_10"])
        res["probes"][str(i)] = pr
    res["delta"] = delta.tolist()
    T.write_json(os.path.join(RES, "e2_history.json"), res)


if __name__ == "__main__":
    main()
