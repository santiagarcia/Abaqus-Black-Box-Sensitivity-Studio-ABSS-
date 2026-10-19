"""Experiment: tangent at converged increment of a NLGEOM+plastic T2D2 truss.

Compares (a) *MATRIX GENERATE in a perturbation step after *RESTART, READ ... END STEP,
with (b) forward finite differences of reaction forces from restarted all-DOF-prescribed
probe steps. Requires t3_base (restart files) to exist. Run: abaqus python exp_truss_restart.py [INC]
"""
import json
import sys

from scout_lib import read_fil, read_mtx, run_job

INC = int(sys.argv[1]) if len(sys.argv) > 1 else 7
base = [r for r in read_fil("t3_base.fil") if r["step"] == 1 and r["inc"] == INC][0]
u0 = base["U"][3]

HEAD = """*HEADING
probe restart
*RESTART, READ, STEP=1, INC=%d, END STEP
""" % INC

def probe_inp(name, u):
    s = HEAD + """*FILE FORMAT, ASCII
*STEP, NAME=PROBE, NLGEOM=YES
*STATIC, DIRECT
1.0, 1.0
*BOUNDARY, OP=NEW
FIX, 1, 2
3, 1, 1, %.17e
3, 2, 2, %.17e
*NODE FILE
U, RF
*EL FILE
S, PE, LE
*END STEP
""" % (u[0], u[1])
    open(name + ".inp", "w").write(s)


def mg_inp(name):
    s = HEAD + """*STEP, NAME=MG
*MATRIX GENERATE, STIFFNESS
*MATRIX OUTPUT, STIFFNESS, FORMAT=MATRIX INPUT
*END STEP
"""
    open(name + ".inp", "w").write(s)


def last_rf(job):
    incs = read_fil(job + ".fil")
    r = incs[-1]
    return r, [r["RF"][3][0], r["RF"][3][1]]

out = {"inc": INC, "u_star": u0}
# (a) matrix generate at restarted state
job = "t3_mg_i%d" % INC
mg_inp(job)
rc, tail = run_job(job, oldjob="t3_base")
out["mg_rc"] = [rc, tail]
K = read_mtx(job + "_STIF2.mtx")
Kmg = [[K.get((3, a, 3, b), 0.0) for b in (1, 2)] for a in (1, 2)]
out["K_matrix_generate"] = Kmg

# (b) FD of RF
job0 = "t3_p0_i%d" % INC
probe_inp(job0, u0)
out["p0_rc"] = list(run_job(job0, oldjob="t3_base"))
r0, rf0 = last_rf(job0)
out["rf0"] = rf0
out["rf_base_inc"] = base["RF"].get(3)
fd = {}
for eps in (1e-4, 1e-5, 1e-6, 1e-7):
    for lab, v in (("+e1", (1.0, 0.0)), ("-e2", (0.0, -1.0))):
        j = "t3_p_i%d_%s_%g" % (INC, lab.replace("+", "p").replace("-", "m"), eps)
        j = j.replace("-", "m").replace(".", "d")
        probe_inp(j, (u0[0] + eps * v[0], u0[1] + eps * v[1]))
        rc, tail = run_job(j, oldjob="t3_base")
        r, rf = last_rf(j)
        col = [(rf[k] - rf0[k]) / eps for k in range(2)]
        if lab == "-e2":
            col = [-c for c in col]  # K[:,2] = -(dRF/d(-e2))... sign: d/d(+e2) = -d/d(-e2)
        fd["%s_eps%g" % (lab, eps)] = {"col": col, "rc": rc, "peeq2": r["EL"].get((2, 1), {}).get("PE")}
out["fd"] = fd
json.dump(out, open("exp_truss_restart_i%d.json" % INC, "w"), indent=1)
print(json.dumps(out, indent=1))
