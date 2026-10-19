"""Experiment: 2x2 CPS4 patch. Full tangent K (all 18 DOFs, no BC) from
(1) *ELEMENT MATRIX OUTPUT in the general step (assembled here),
(2) *MATRIX GENERATE after *RESTART, READ ... END STEP,
(3) forward FD of reaction forces from restarted all-DOF-prescribed probe jobs,
(4) [linear case only] independent plane-stress 2x2-Gauss CPS4 stiffness (validation-only code).

Usage: abaqus python exp_cps4.py CASE INC EPS
  CASE = lin (NLGEOM=NO, elastic) | nl (NLGEOM=YES, elastic) | pl (NLGEOM=YES, Mises plastic)
"""
import json
import math
import sys
import time

from emo_lib import assemble, read_emo
from scout_lib import read_fil, read_mtx, run_job

CASE = sys.argv[1]
INC = int(sys.argv[2])
EPS = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-7
E, NU, T = 1000.0, 0.3, 1.0

nodes = {}
k = 1
for j in range(3):
    for i in range(3):
        nodes[k] = (0.5 * i, 0.5 * j)
        k += 1
elems = {1: (1, 2, 5, 4), 2: (2, 3, 6, 5), 3: (4, 5, 8, 7), 4: (5, 6, 9, 8)}
DOFS = [(n, d) for n in sorted(nodes) for d in (1, 2)]

nlgeom = "NO" if CASE == "lin" else "YES"
plastic = "*PLASTIC\n20., 0.\n40., 0.2\n" if CASE == "pl" else ""
load = {"lin": 10.0, "nl": 60.0, "pl": 30.0}[CASE]
base = "c4_%s_base" % CASE


def write_base():
    s = "*HEADING\nScout 2x2 CPS4 patch case %s\n*NODE, NSET=ALLN\n" % CASE
    s += "".join("%d, %r, %r\n" % (n, x, y) for n, (x, y) in nodes.items())
    s += "*ELEMENT, TYPE=CPS4, ELSET=ALLE\n"
    s += "".join("%d, %d, %d, %d, %d\n" % ((e,) + c) for e, c in elems.items())
    s += """*SOLID SECTION, ELSET=ALLE, MATERIAL=MAT
%r
*MATERIAL, NAME=MAT
*ELASTIC
%r, %r
%s*NSET, NSET=BOT
1, 2, 3
*NSET, NSET=TOP
7, 8, 9
*BOUNDARY
BOT, 2, 2
1, 1, 1
*RESTART, WRITE, FREQUENCY=1
*FILE FORMAT, ASCII
*STEP, NAME=LOAD, NLGEOM=%s, INC=100
*STATIC, DIRECT
0.2, 1.0
*CLOAD
7, 2, %r
8, 2, %r
9, 2, %r
9, 1, %r
*NODE FILE, FREQUENCY=1
U, RF
*EL FILE, FREQUENCY=1
S, PE, LE
*ELEMENT MATRIX OUTPUT, ELSET=ALLE, STIFFNESS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=%s_emo, FREQUENCY=1
*OUTPUT, FIELD, FREQUENCY=1
*NODE OUTPUT
U, RF
*ELEMENT OUTPUT
S, PE, PEEQ, LE
*END STEP
""" % (T, E, NU, plastic, nlgeom, load / 4, load / 2, load / 4, load / 10, base)
    open(base + ".inp", "w").write(s)


HEAD = "*HEADING\nprobe\n*RESTART, READ, STEP=1, INC=%d, END STEP\n" % INC


def probe(job, u):
    s = HEAD + "*FILE FORMAT, ASCII\n*STEP, NAME=PROBE, NLGEOM=%s\n*STATIC, DIRECT\n1.0, 1.0\n*BOUNDARY, OP=NEW\n" % nlgeom
    s += "".join("%d, %d, %d, %.17e\n" % (n, d, d, u[(n, d)]) for (n, d) in DOFS)
    s += "*NODE FILE\nU, RF\n*END STEP\n"
    open(job + ".inp", "w").write(s)
    rc, tail = run_job(job, oldjob=base)
    r = read_fil(job + ".fil")[-1]
    return rc, {(n, d): r["RF"][n][d - 1] for (n, d) in DOFS}


def gauss_k():
    """Independent plane-stress full-integration bilinear quad K (small strain), validation only."""
    D = E / (1 - NU ** 2)
    C = [[D, D * NU, 0], [D * NU, D, 0], [0, 0, D * (1 - NU) / 2]]
    G = {}
    g = 1 / math.sqrt(3)
    for e, conn in elems.items():
        xy = [nodes[n] for n in conn]
        Ke = [[0.0] * 8 for _ in range(8)]
        for xi in (-g, g):
            for eta in (-g, g):
                dN = [(-(1 - eta) / 4, -(1 - xi) / 4), ((1 - eta) / 4, -(1 + xi) / 4),
                      ((1 + eta) / 4, (1 + xi) / 4), (-(1 + eta) / 4, (1 - xi) / 4)]
                J = [[sum(dN[a][r] * xy[a][c] for a in range(4)) for c in range(2)] for r in range(2)]
                det = J[0][0] * J[1][1] - J[0][1] * J[1][0]
                Ji = [[J[1][1] / det, -J[0][1] / det], [-J[1][0] / det, J[0][0] / det]]
                dX = [(Ji[0][0] * a + Ji[0][1] * b, Ji[1][0] * a + Ji[1][1] * b) for a, b in dN]
                B = [[0.0] * 8 for _ in range(3)]
                for a in range(4):
                    B[0][2 * a] = dX[a][0]
                    B[1][2 * a + 1] = dX[a][1]
                    B[2][2 * a] = dX[a][1]
                    B[2][2 * a + 1] = dX[a][0]
                for p in range(8):
                    for q in range(8):
                        Ke[p][q] += sum(B[i][p] * C[i][j] * B[j][q] for i in range(3) for j in range(3)) * det * T
        keys = [(n, d) for n in conn for d in (1, 2)]
        for a, ka in enumerate(keys):
            for b, kb in enumerate(keys):
                G[ka + kb] = G.get(ka + kb, 0.0) + Ke[a][b]
    return G


def mat(G):
    return [[G.get(a + b, 0.0) for b in DOFS] for a in DOFS]


def relmax(A, B):
    s = max(abs(x) for row in B for x in row)
    return max(abs(A[i][j] - B[i][j]) for i in range(len(A)) for j in range(len(A))) / s


t0 = time.time()
write_base()
import os
if not os.path.exists(base + ".fil"):
    print("base", run_job(base))
incs = read_fil(base + ".fil")
r = [x for x in incs if x["step"] == 1 and x["inc"] == INC][0]
ustar = {(n, d): r["U"][n][d - 1] for (n, d) in DOFS}
res = {"case": CASE, "inc": INC, "eps": EPS}
peeq = [v.get("PE") for v in r["EL"].values()]
res["PE_at_inc"] = peeq[:2]

Kemo = mat(assemble(read_emo(base + "_emo.mtx")[(1, INC)]))

mg = "c4_%s_mg_i%d" % (CASE, INC)
open(mg + ".inp", "w").write(HEAD + "*STEP, NAME=MG\n*MATRIX GENERATE, STIFFNESS\n*MATRIX OUTPUT, STIFFNESS, FORMAT=MATRIX INPUT\n*END STEP\n")
res["mg_run"] = run_job(mg, oldjob=base)
Kmg_raw = read_mtx(mg + "_STIF2.mtx")
Kmg = mat(Kmg_raw)
bcdiag = [a for a in DOFS if abs(Kmg_raw.get(a + a, 0)) > 1e30]
res["mg_penalty_dofs"] = bcdiag
Kmg_nobc = [[(Kemo[i][j] if (DOFS[i] in bcdiag and i == j) else Kmg[i][j]) for j in range(18)] for i in range(18)]

rc0, rf0 = probe("c4_%s_p0_i%d" % (CASE, INC), ustar)
res["rf0_maxabs"] = max(abs(v) for v in rf0.values())
Kfd = [[0.0] * 18 for _ in range(18)]
for j, dj in enumerate(DOFS):
    u = dict(ustar)
    u[dj] += EPS
    rc, rf = probe(("c4_%s_p_i%d_%d_%d" % (CASE, INC, dj[0], dj[1])), u)
    for i, di in enumerate(DOFS):
        Kfd[i][j] = (rf[di] - rf0[di]) / EPS
res["relmax_emo_vs_fd"] = relmax(Kemo, Kfd)
res["relmax_mg_vs_emo_offBCdiag"] = relmax(Kmg_nobc, Kemo)
res["fd_asym"] = relmax(Kfd, [list(x) for x in zip(*Kfd)])
if CASE == "lin":
    Kg = mat(gauss_k())
    res["relmax_emo_vs_gauss"] = relmax(Kemo, Kg)
    res["relmax_fd_vs_gauss"] = relmax(Kfd, Kg)
res["K_emo_row_9_2"] = Kemo[17]
res["K_fd_row_9_2"] = Kfd[17]
res["wall_s"] = time.time() - t0
json.dump(res, open("exp_cps4_%s_i%d.json" % (CASE, INC), "w"), indent=1)
print(json.dumps({k: v for k, v in res.items() if not k.startswith("K_")}, indent=1))

