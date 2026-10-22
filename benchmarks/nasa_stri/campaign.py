"""Resumable, interleaved ABSS vs FAFD benchmark driver.

A plan is a JSON list of cases: {"id", "deck", "params": [...], "methods": ["abss","fafd"], "jobs": [1, 8],
"repeats": n, "extra": [...cli flags...], "extra_<method>": [...], "bin": "<snapshot dir>", "user": "<library>"}.
Methods starting with "fafd" run `abss-cli fafd`; all others run `abss-cli run` (e.g. "abss_legacy" with
"extra_abss_legacy": ["--no-overlap"]). "bin_<method>" selects another frozen binary for one method.
Every (case, method, jobs, repeat) writes <out>/<id>__<method>_j<jobs>_r<k>.{json,log,abss}; existing JSON = done
(checkpoint), so an interrupted campaign resumes where it stopped. Runs are interleaved: repeat-major, then case,
then method, so slow drifts (licence server, thermal state, other users) spread over both methods equally.

Usage:  python campaign.py <plan.json> <outdir> [--dry]
"""
import json
import os
import shutil
import subprocess
import sys
import time


def main():
    plan_path, out = sys.argv[1], sys.argv[2]
    dry = "--dry" in sys.argv
    # %BIN% in a plan = the ABSS_BENCH_BIN environment variable (a frozen copy of the build's bin directory)
    text = open(plan_path, encoding="utf-8-sig").read().replace("%BIN_V1%", os.environ.get("ABSS_BENCH_BIN_V1", "").replace("\\", "/"))
    plan = json.loads(text.replace("%BIN%", os.environ.get("ABSS_BENCH_BIN", "").replace("\\", "/")))
    os.makedirs(out, exist_ok=True)
    runs = []
    max_rep = max(c.get("repeats", 1) for c in plan)
    for r in range(1, max_rep + 1):
        for c in plan:
            if r > c.get("repeats", 1):
                continue
            for jobs in c.get("jobs", [1]):
                methods = c.get("methods", ["abss", "fafd"])
                # alternate method order between repeats
                for m in (methods if r % 2 else list(reversed(methods))):
                    runs.append((c, m, jobs, r))
    print(f"{len(runs)} runs planned")
    for c, m, jobs, r in runs:
        tag = f"{c['id']}__{m}_j{jobs}_r{r}"
        js = os.path.join(out, tag + ".json")
        if os.path.exists(js):
            continue
        exe = os.path.join(c.get("bin_" + m, c["bin"]), "abss-cli.exe" if os.name == "nt" else "abss-cli")
        scratch = os.path.join(os.environ.get("TEMP", "/tmp"), "abss_bench", tag)
        if not dry:  # never reuse files (or lock files) of an earlier, interrupted run
            shutil.rmtree(scratch, ignore_errors=True)
        cmd = [exe, "fafd" if m.startswith("fafd") else "run", c["deck"], "--params", ",".join(c["params"]),
               "--jobs", str(jobs), "--scratch", scratch,
               "--out", os.path.join(out, tag + ".abss"), "--profile", js + ".partial"]
        if c.get("user"):
            cmd += ["--user", c["user"]]
        cmd += c.get("extra", []) + (c.get("extra_" + m, []))
        print(time.strftime("%H:%M:%S"), tag, flush=True)
        if dry:
            print("   ", " ".join(cmd))
            continue
        t0 = time.time()
        with open(os.path.join(out, tag + ".log"), "w") as log:
            log.write("# " + " ".join(cmd) + "\n")
            log.flush()
            rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT)
        wall = time.time() - t0
        if os.path.exists(js + ".partial"):
            d = json.load(open(js + ".partial"))
            d["driver"] = {"exit_code": rc, "process_wall_seconds": wall, "repeat": r, "case": c["id"],
                           "started": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t0))}
            json.dump(d, open(js, "w"), indent=2)
            os.remove(js + ".partial")
        else:
            json.dump({"driver": {"exit_code": rc, "process_wall_seconds": wall, "repeat": r, "case": c["id"],
                                  "failed": True}, "method": m}, open(js, "w"), indent=2)
        print(f"    exit {rc}, {wall:.1f} s", flush=True)


if __name__ == "__main__":
    main()
