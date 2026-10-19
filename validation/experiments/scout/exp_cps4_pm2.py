"""Forward vs backward FD columns for the plastic CPS4 patch (case pl) at INC,
with element output in probes, to see whether probes cause plastic flow and
which one-sided tangent *ELEMENT MATRIX OUTPUT matches. Also tries a large
uniform-stretch direction v (all top nodes +y) as a directional derivative K.v.
Usage: abaqus python exp_cps4_pm.py INC EPS
"""
import json
import sys

from emo_lib import assemble, read_emo
from scout_lib import read_fil, run_job

INC = int(sys.argv[1])
EPS = float(sys.argv[2])
base = "c4_pl_base"
DOFS = [(n, d) for n in range(1, 10) for d in (1, 2)]
r = [x for x in read_fil(base + ".fil") if x["inc"] == INC][0]
ustar = {(n, d): r["U"][n][d - 1] for (n, d) in DOFS}
HEAD = "*HEADING\nprobe\n*RESTART, READ, STEP=1, INC=%d, END STEP\n" % INC


def probe(job, u):
    job = job.replace(".", "d").replace("-", "m")
    s = HEAD + "*FILE FORMAT, ASCII\n*STEP, NAME=PROBE, NLGEOM=YES\n*STATIC, DIRECT\n1.0, 1.0\n*BOUNDARY, OP=NEW\n"
    s += "".join("%d, %d, %d, %.17e\n" % (n, d, d, u[(n, d)]) for (n, d) in DOFS)
    s += "*NODE FILE\nU, RF\n*EL FILE\nPE\n*END STEP\n"
    open(job + ".inp", "w").write(s)
    run_job(job, oldjob=base)
    rr = read_fil(job + ".fil")[-1]
    rf = {(n, d): rr["RF"][n][d - 1] for (n, d) in DOFS}
    peeq = [v["PE"][-1] for k, v in sorted(rr["EL"].items())]
    return rf, peeq


G = assemble(read_emo(base + "_emo.mtx")[(1, INC)])
Kemo = [[G.get(a + b, 0.0) for b in DOFS] for a in DOFS]
rf0, pq0 = probe("c4pm2_e%g_" % EPS + "0_i%d" % INC, ustar)
out = {"inc": INC, "eps": EPS, "peeq0": pq0, "cols": {}}
vdirs = {"top_stretch": {(7, 2): 1.0, (8, 2): 1.0, (9, 2): 1.0}}
for name, v in vdirs.items():
    Kv = [sum(Kemo[i][j] * v.get(DOFS[j], 0.0) for j in range(18)) for i in range(18)]
    for sgn in (1, -1):
        u = {k: ustar[k] + sgn * EPS * v.get(k, 0.0) for k in DOFS}
        rf, pq = probe("c4pm2_e%g_" % EPS + "%s_%s_i%d" % (name, "p" if sgn > 0 else "m", INC), u)
        fd = [(rf[k] - rf0[k]) / (sgn * EPS) for k in DOFS]
        err = max(abs(fd[i] - Kv[i]) for i in range(18)) / max(abs(x) for x in Kv)
        out["cols"]["%s_%s" % (name, "fwd" if sgn > 0 else "bwd")] = {
            "relerr_vs_emo": err, "dpeeq": [a - b for a, b in zip(pq, pq0)]}
json.dump(out, open("exp_cps4_pm2_i%d_e%g.json" % (INC, EPS), "w"), indent=1)
print(json.dumps(out, indent=1))


