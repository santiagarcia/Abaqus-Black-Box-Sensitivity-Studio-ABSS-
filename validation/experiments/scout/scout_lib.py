"""Scout helpers for Abaqus 2024 experiments (run with `abaqus python`).

Parses ASCII results files (*FILE FORMAT, ASCII) and MATRIX INPUT .mtx files,
and launches Abaqus jobs sequentially. Experimental tooling, not product code.
"""
import os
import subprocess

ABQ = r"C:\SIMULIA\Commands\abaqus.bat"


def run_job(job, oldjob=None, cwd=None, extra=()):
    """Run an Abaqus/Standard job interactively; return (returncode, tail of log)."""
    cmd = 'cmd /c "%s job=%s interactive ask_delete=OFF cpus=1' % (ABQ, job)
    if oldjob:
        cmd += " oldjob=%s" % oldjob
    for e in extra:
        cmd += " " + e
    cmd += '"'
    p = subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    return p.returncode, out.strip().splitlines()[-1:] if out.strip() else []


def _fil_records(path):
    """Yield lists of tokens (ints, floats, strings) for each ASCII .fil record."""
    with open(path, "r") as f:
        raw = "".join(line.rstrip("\r\n").ljust(80) for line in f)
    for rec in raw.split("*")[1:]:
        toks = []
        i = 0
        n = len(rec)
        while i < n:
            c = rec[i]
            if c == "I":
                w = int(rec[i + 1:i + 3])
                toks.append(int(rec[i + 3:i + 3 + w]))
                i += 3 + w
            elif c == "D":
                toks.append(float(rec[i + 1:i + 23].replace("D", "E")))
                i += 23
            elif c == "A":
                toks.append(rec[i + 1:i + 9])
                i += 9
            elif c == " ":
                i += 1  # padding
            else:
                raise ValueError("bad token %r at %d in %r" % (c, i, rec[:80]))
        if toks:
            yield toks


def read_fil(path):
    """Return list of increments: dict(step, inc, time, U{node:[...]}, RF{...}, elem{...}).

    Record keys (Abaqus results file format): 2000 increment start, 1 element header,
    11 S, 22 PE, 89 LE, 101 U, 104 RF, 5 SDV.
    """
    incs = []
    cur = None
    elem_key = None
    names = {11: "S", 22: "PE", 89: "LE", 5: "SDV", 21: "E", 401: "SP"}
    for t in _fil_records(path):
        key = t[1]
        data = t[2:]
        if key == 2000:
            # total time, step time, ..., step, inc
            cur = {"total_time": data[0], "step_time": data[1], "step": data[5],
                   "inc": data[6], "U": {}, "RF": {}, "EL": {}}
            incs.append(cur)
        elif cur is None:
            continue
        elif key == 101:
            cur["U"][data[0]] = data[1:]
        elif key == 104:
            cur["RF"][data[0]] = data[1:]
        elif key == 1:
            elem_key = (data[0], data[1])  # element, int point
        elif key in names and elem_key is not None:
            cur["EL"].setdefault(elem_key, {})[names[key]] = data
    return incs


def read_mtx(path):
    """Read MATRIX INPUT format lines 'n1,d1, n2,d2, value' -> dict[(n1,d1,n2,d2)] (full symmetric)."""
    K = {}
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("*"):
                continue
            p = [x.strip() for x in s.split(",")]
            n1, d1, n2, d2 = (int(x) for x in p[:4])
            v = float(p[4])
            K[(n1, d1, n2, d2)] = v
            K[(n2, d2, n1, d1)] = v
    return K
