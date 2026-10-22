"""ABSS vs FAFD agreement with near-zero-safe metrics (reads .abss files directly; no Abaqus, no ABSS build needed).

For every derivative dF/da (matched by parameter id) and field F, over increments 1..n and every location:
  max_ref      = max |FAFD|                        max_err = max |ABSS - FAFD|
  natural      = max |F_nominal| / s,   s = |a| (or the parameter's scale when a = 0: half its bound interval, else 1)
  err_natural  = max_err / natural                 (absolute error in units of the derivative's natural size)
  err_scaled   = max_err / max(max_ref, 0.05 * natural)   (the same floor as ABSS's validation gate)
  err_rel_ref  = max_err / max_ref                 (no floor; meaningless where the true derivative is ~0)

Usage:  python accuracy.py <reference.abss> <test.abss> [--json out.json]
        python accuracy.py --all <results dir> ...   (every <case>__abss*_j*_r* run against its FAFD partner)
"""
import glob
import json
import math
import os
import struct
import sys


def load(path):
    with open(path, "rb") as f:
        f.read(8)
        (n,) = struct.unpack("<Q", f.read(8))
        m = json.loads(f.read(n))
        base = f.tell()
        series = {}
        for s in m["series"]:
            f.seek(base + s["data"]["offset"])
            cnt = s["data"]["count"]
            series[(s["field"], tuple(s["params"]), s["frame"])] = struct.unpack(f"<{cnt}d", f.read(8 * cnt))
    return m, series


def scale_of(p):
    a = abs(p.get("nominal", 0.0))
    if a > 0 and math.isfinite(a):
        return a
    lo, hi = p.get("lower"), p.get("upper")
    if isinstance(lo, (int, float)) and isinstance(hi, (int, float)) and -1e299 < lo < hi < 1e299:
        return 0.5 * (hi - lo)
    return 1.0


def compare(ref_path, test_path):
    rm, rs = load(ref_path)
    tm, ts = load(test_path)
    tid = {p["id"]: i for i, p in enumerate(tm["parameters"])}
    tfield = {f["name"]: i for i, f in enumerate(tm["fields"])}
    nframes = len(rm["frames"])
    out = []
    for pi, p in enumerate(rm["parameters"]):
        if p["id"] not in tid:
            continue
        qi = tid[p["id"]]
        s = scale_of(p)
        for fi, fd in enumerate(rm["fields"]):
            if fd["name"] not in tfield or not any((fi, (pi,), fr) in rs for fr in range(1, nframes)):
                continue
            gi = tfield[fd["name"]]
            fmax = max_ref = max_err = 0.0
            n = 0
            missing = 0
            for fr in range(1, nframes):
                nom = rs.get((fi, (), fr))
                if nom:
                    fmax = max(fmax, max((abs(x) for x in nom if math.isfinite(x)), default=0.0))
                a, b = rs.get((fi, (pi,), fr)), ts.get((gi, (qi,), fr))
                if a is None and b is None:
                    continue
                if a is None or b is None or len(a) != len(b):
                    missing += 1
                    continue
                for x, y in zip(a, b):
                    if math.isfinite(x) and math.isfinite(y):
                        max_ref = max(max_ref, abs(x))
                        max_err = max(max_err, abs(x - y))
                        n += 1
            natural = fmax / s if s > 0 else 0.0
            div = lambda num, den: (num / den) if den > 0 else (0.0 if num == 0 else None)
            out.append({"parameter_id": p["id"], "field": fd["name"], "values_compared": n, "frames_missing": missing,
                        "max_ref": max_ref, "max_err": max_err, "nominal_field_max": fmax, "parameter_scale": s,
                        "natural_scale": natural, "err_natural": div(max_err, natural),
                        "err_scaled_floor": div(max_err, max(max_ref, 0.05 * natural)),
                        "err_rel_ref": div(max_err, max_ref)})
    return out


def partner(path):
    d, tag = os.path.dirname(path), os.path.basename(path)[:-5]
    case, rest = tag.split("__")
    jobs, rep = rest.rsplit("_j", 1)[1].split("_r")
    for r in (rep, "1"):
        f = os.path.join(d, f"{case}__fafd_j{jobs}_r{r}.abss")
        if os.path.exists(f):
            return f
    return None


def main():
    a = sys.argv[1:]
    if a and a[0] == "--all":
        for d in a[1:]:
            for t in sorted(glob.glob(os.path.join(d, "*__abss*_j*_r*.abss"))):
                ref = partner(t)
                out = os.path.join(d, "acc_" + os.path.basename(t)[:-5] + ".json")
                if not ref or os.path.exists(out):
                    continue
                json.dump({"reference": os.path.basename(ref), "test": os.path.basename(t), "rows": compare(ref, t)},
                          open(out, "w"), indent=1)
                print(out)
        return
    rows = compare(a[0], a[1])
    if "--json" in a:
        json.dump(rows, open(a[a.index("--json") + 1], "w"), indent=1)
    fmt = lambda v: "   n/a  " if v is None else f"{v:9.2e}"
    print(f"{'parameter':34} {'field':8} {'max|ref|':>9} {'max|err|':>9} {'err/nat':>9} {'floored':>9} {'err/ref':>9}")
    for r in rows:
        print(f"{r['parameter_id'][:34]:34} {r['field'][:8]:8} {r['max_ref']:9.2e} {r['max_err']:9.2e} "
              f"{fmt(r['err_natural'])} {fmt(r['err_scaled_floor'])} {fmt(r['err_rel_ref'])}")


if __name__ == "__main__":
    main()
