# -*- coding: utf-8 -*-
"""Small helpers shared by the Curie oracle experiments.

Run inside the Abaqus Python interpreter (``abaqus python``), which provides
numpy. Jobs are run sequentially, one CPU, in a per-experiment work folder.
"""
from __future__ import print_function

import json
import os
import subprocess
import sys

ABAQUS = os.environ.get("ABSS_ABAQUS_CMD", r"C:\SIMULIA\Commands\abaqus.bat")
HERE = os.path.dirname(os.path.abspath(__file__))


def run_job(workdir, name, inp_text, extra=None, check=True):
    """Write NAME.inp in workdir and run it to completion (interactive)."""
    if not os.path.isdir(workdir):
        os.makedirs(workdir)
    inp = os.path.join(workdir, name + ".inp")
    with open(inp, "w") as fh:
        fh.write(inp_text)
    for ext in (".lck",):
        p = os.path.join(workdir, name + ext)
        if os.path.exists(p):
            os.remove(p)
    cmd = [ABAQUS, "job=" + name, "interactive", "ask_delete=OFF", "cpus=1"]
    if extra:
        cmd += list(extra)
    proc = subprocess.run(
        ["cmd", "/c"] + cmd, cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    log = proc.stdout.decode("utf-8", "replace")
    with open(os.path.join(workdir, name + ".runlog.txt"), "w") as fh:
        fh.write(log)
    sta = os.path.join(workdir, name + ".sta")
    ok = False
    if os.path.exists(sta):
        with open(sta) as fh:
            ok = "COMPLETED SUCCESSFULLY" in fh.read()
    if check and not ok:
        raise RuntimeError("Abaqus job %s failed; see %s" % (name, workdir))
    return ok


def dump(workdir, name, fields="U,RF,CF,S,PEEQ,E,PE", mesh=False):
    """Run odb_dump.py on NAME.odb (a fresh abaqus python process)."""
    odb = os.path.join(workdir, name + ".odb")
    out = os.path.join(workdir, name + ".json")
    cmd = [ABAQUS, "python", os.path.join(HERE, "odb_dump.py"), odb, out, "--fields", fields]
    if mesh:
        cmd.append("--mesh")
    subprocess.run(["cmd", "/c"] + cmd, cwd=workdir, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with open(out) as fh:
        return json.load(fh)


def nodal(frame, fname, ncomp=None):
    """{node_label: np.array(components)} for a nodal field of a frame dict."""
    import numpy as np

    f = frame["fields"][fname]
    out = {}
    for inst, label, ip, sp, vals, _node in f["values"]:
        v = np.array(vals, dtype=float)
        if ncomp is not None:
            v = v[:ncomp]
        out[int(label)] = v
    return out


def ipfield(frame, fname):
    """{(element, ip): np.array(components)} for an integration-point field."""
    import numpy as np

    f = frame["fields"][fname]
    return dict(((int(l), int(ip)), np.array(v, dtype=float))
                for inst, l, ip, sp, v, _n in f["values"])


def write_json(path, obj):
    with open(path, "w") as fh:
        json.dump(obj, fh, indent=2, sort_keys=True, default=float)


def sta_increments(workdir, name):
    """Parse the .sta file.

    Returns dicts with step, inc, att, severe, equil, iters, total_time,
    step_time, dt (the standard Abaqus/Standard .sta columns).
    """
    rows = []
    with open(os.path.join(workdir, name + ".sta")) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) >= 9 and parts[0].isdigit() and parts[1].isdigit():
                try:
                    rows.append(dict(step=int(parts[0]), inc=int(parts[1]), att=parts[2],
                                     severe=int(parts[3]), equil=int(parts[4]),
                                     iters=int(parts[5]), total_time=float(parts[6]),
                                     step_time=float(parts[7]), dt=float(parts[8])))
                except ValueError:
                    pass
    return rows


def cps4_stiffness(nodes, elems, E, nu, t):
    """Small-strain plane-stress CPS4 (2x2 Gauss) stiffness, VALIDATION REFERENCE ONLY.

    nodes: {label: (x, y)} with labels 1..n; elems: {label: (n1, n2, n3, n4)}.
    DOF index = 2*(node-1) + (dof-1). Verified equal to Abaqus *MATRIX GENERATE
    for CPS4 to 4e-15 relative (experiment E1).
    """
    import numpy as np

    D = E / (1 - nu ** 2) * np.array([[1, nu, 0], [nu, 1, 0], [0, 0, (1 - nu) / 2]])
    n = len(nodes)
    K = np.zeros((2 * n, 2 * n))
    g = 1 / np.sqrt(3.0)
    xi_n = np.array([-1, 1, 1, -1.0])
    et_n = np.array([-1, -1, 1, 1.0])
    for e, conn in elems.items():
        X = np.array([nodes[c] for c in conn], dtype=float)
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


if __name__ == "__main__":
    print(sys.version)
