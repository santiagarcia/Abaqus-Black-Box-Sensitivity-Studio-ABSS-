"""Flatten benchmark profile JSON files into one CSV row per run (raw data for every figure and table).

Usage:  python analyze.py <results dir> [<results dir> ...] --out summary.csv
"""
import csv
import glob
import json
import os
import sys

COLUMNS = [
    "stage", "case", "method", "jobs", "repeat", "exit_code", "failed", "deck", "elements", "material_points", "dofs",
    "free_dofs", "increments", "n_params", "max_order", "overlap_waves", "fuse_commit_probe", "suppress_odb_output",
    "t_setup", "t_grid_setup", "t_nominal", "t_sensitivity", "t_workflow", "t_validation", "t_serialize", "t_total",
    "t_process_wall",
    "abaqus_jobs", "abaqus_jobs_sensitivity", "abaqus_job_seconds_summed", "job_driver_licence_s", "job_pre_s",
    "job_standard_s", "job_wrapup_s", "job_wait_slot_s", "job_parse_s", "job_input_generation_s",
    "engine_factorize_s", "engine_solve_s", "engine_tangent_assembly_s", "gmres_rhs", "gmres_matvecs",
    "krylov_increments", "krylov_not_converged", "probed_tangent_increments", "worst_tangent_check",
    "fused_probes_used", "oracle_evaluations", "fafd_analyses", "fafd_failed", "quality_status", "worst_fd_rel_error",
    "cmp_worst_scaled_error_vs_fafd", "cmp_worst_field", "cmp_fields", "cmp_frames_missing",
    "acc_worst_err_natural", "acc_worst_err_scaled_floor", "acc_worst_where", "acc_values_compared",
    "overlaps_build_or_check",
]


def attach_accuracy(r, path):
    """Near-zero-safe agreement with FAFD from acc_<run>.json (accuracy.py): worst error over every derivative and
    field, in units of the derivative's natural size max|F|/|a| and with the validation floor."""
    acc = os.path.join(os.path.dirname(path), "acc_" + os.path.basename(path)[:-5] + ".json")
    if not os.path.exists(acc):
        return
    rows = json.load(open(acc))["rows"]
    nat = [(x["err_natural"], x["parameter_id"] + " " + x["field"]) for x in rows if x["err_natural"] is not None]
    flo = [x["err_scaled_floor"] for x in rows if x["err_scaled_floor"] is not None]
    if nat:
        r["acc_worst_err_natural"], r["acc_worst_where"] = max(nat)
    if flo:
        r["acc_worst_err_scaled_floor"] = max(flo)
    r["acc_values_compared"] = sum(x["values_compared"] for x in rows)


def windows():
    """(start, end) datetimes of builds and checks run on the machine during the campaign (results/BUILD_WINDOWS.txt)."""
    import datetime as dt
    import re
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results", "BUILD_WINDOWS.txt")
    out = []
    if os.path.exists(p):
        for line in open(p, encoding="utf-8-sig"):
            m = re.search(r"(\d\d:\d\d:\d\d)-(\d\d:\d\d:\d\d)", line)
            if m:
                day = "2026-10-21"
                out.append((dt.datetime.fromisoformat(f"{day}T{m.group(1)}"), dt.datetime.fromisoformat(f"{day}T{m.group(2)}")))
    return out


WINDOWS = windows()


def overlaps_window(d):
    import datetime as dt
    drv = d.get("driver", {})
    if "started" not in drv:
        return ""
    s = dt.datetime.fromisoformat(drv["started"])
    e = s + dt.timedelta(seconds=float(drv.get("process_wall_seconds", 0)))
    return "1" if any(s < we and e > ws for ws, we in WINDOWS) else "0"


def attach_comparison(r, path):
    """Worst scaled error (max|ABSS - FAFD| / max|FAFD| per field) of this run's cmp_*.json, if any."""
    d, tag = os.path.dirname(path), os.path.basename(path)[:-5]
    if "__" not in tag:
        return
    case, rest = tag.split("__")
    cmp = os.path.join(d, f"cmp_{case}__{rest}.json")
    if not os.path.exists(cmp):
        return
    rows = json.load(open(cmp))
    errs = [(e["scaled_error"], e["field"]) for e in rows if e.get("scaled_error") is not None]
    if errs:
        worst = max(errs)
        r["cmp_worst_scaled_error_vs_fafd"], r["cmp_worst_field"] = worst[0], worst[1]
    r["cmp_fields"] = "/".join(sorted({e["field"] for e in rows}))
    r["cmp_frames_missing"] = max((e.get("frames_missing", 0) for e in rows), default=0)


def prof(p, key, field="seconds"):
    return p.get(key, {}).get(field, 0)


def row_of(path, stage):
    d = json.load(open(path))
    drv = d.get("driver", {})
    r = {c: "" for c in COLUMNS}
    r.update(stage=stage, case=drv.get("case", os.path.basename(path).split("__")[0]), method=d.get("method", ""),
             repeat=drv.get("repeat", 1), exit_code=drv.get("exit_code", 0), t_process_wall=drv.get("process_wall_seconds", ""))
    tag = os.path.basename(path)[:-5]
    if "__" in tag:
        r["method"] = tag.split("__")[1].rsplit("_j", 1)[0]
        r["jobs"] = int(tag.rsplit("_j", 1)[1].split("_r")[0])
    if drv.get("failed"):
        r["failed"] = 1
        return r
    p = d.get("profile", {})
    e = d.get("elapsed_seconds", {})
    r.update(deck=os.path.basename(d.get("inp", "")), elements=d.get("elements"), material_points=d.get("material_points"),
             dofs=d.get("dofs"), free_dofs=d.get("free_dofs"), increments=d.get("increments"),
             n_params=len(d.get("parameters", [])), max_order=d.get("max_order"), jobs=d.get("max_concurrent_jobs", r["jobs"]),
             overlap_waves=d.get("overlap_waves"), fuse_commit_probe=d.get("fuse_commit_probe"),
             suppress_odb_output=d.get("suppress_odb_output"),
             t_setup=e.get("setup"), t_grid_setup=e.get("grid_setup", ""), t_nominal=e.get("nominal"),
             t_sensitivity=e.get("sensitivity"),
             t_workflow=(e.get("nominal") or 0) + (e.get("sensitivity") or 0) if e.get("sensitivity") is not None else "",
             t_validation=e.get("validation", ""), t_serialize=e.get("serialize", ""), t_total=e.get("total"))
    jobs = {k[5:]: v for k, v in p.items() if k.startswith("jobs.")}
    r["abaqus_jobs"] = sum(v["count"] for v in jobs.values())
    r["abaqus_jobs_sensitivity"] = sum(v["count"] for k, v in jobs.items()
                                       if not k.startswith(("validation", "nominal", "fafd.nominal", "fafd.grid_setup")))
    r["abaqus_job_seconds_summed"] = round(prof(p, "job.process"), 3)
    r["job_driver_licence_s"] = round(prof(p, "job.phase.driver_and_licence"), 3)
    r["job_pre_s"] = round(prof(p, "job.phase.input_processing"), 3)
    r["job_standard_s"] = round(prof(p, "job.phase.analysis"), 3)
    r["job_wrapup_s"] = round(prof(p, "job.phase.wrapup"), 3)
    r["job_wait_slot_s"] = round(prof(p, "job.wait_slot"), 3)
    r["job_parse_s"] = round(prof(p, "job.parse_results"), 3)
    r["job_input_generation_s"] = round(prof(p, "job.input_generation"), 3)
    r["engine_factorize_s"] = round(prof(p, "engine.factorize"), 4)
    r["engine_solve_s"] = round(prof(p, "engine.direct_solve") + prof(p, "stage.gmres_solve"), 4)
    r["engine_tangent_assembly_s"] = round(prof(p, "engine.tangent_assembly"), 4)
    r["gmres_rhs"] = prof(p, "engine.gmres_rhs", "count")
    r["fused_probes_used"] = prof(p, "engine.fused_probes_used", "count")
    fr = d.get("frames", [])
    r["gmres_matvecs"] = sum(f.get("krylov_matvecs", 0) for f in fr)
    r["krylov_increments"] = sum(1 for f in fr if f.get("krylov"))
    r["krylov_not_converged"] = sum(1 for f in fr if f.get("krylov") and not f.get("krylov_converged", True))
    r["probed_tangent_increments"] = sum(1 for f in fr if f.get("probed_tangent"))
    tc = [f.get("tangent_check", -1) for f in fr if f.get("tangent_check", -1) >= 0]
    r["worst_tangent_check"] = max(tc) if tc else ""
    r["oracle_evaluations"] = d.get("oracle_evaluations", "")
    r["fafd_analyses"] = d.get("fafd_analyses", "")
    r["fafd_failed"] = d.get("fafd_failed", "")
    q = d.get("quality", {})
    st = sorted({v.get("status") for v in q.values()})
    r["quality_status"] = "/".join(s for s in st if s)
    errs = [v.get("fd_rel_error", -1) for v in q.values() if v.get("fd_rel_error", -1) >= 0]
    r["worst_fd_rel_error"] = max(errs) if errs else ""
    r["failed"] = int(r["exit_code"] not in (0, "0") or bool(d.get("fafd_failed")))
    attach_comparison(r, path)
    attach_accuracy(r, path)
    r["overlaps_build_or_check"] = overlaps_window(d)
    return r


def main():
    args = sys.argv[1:]
    out = args[args.index("--out") + 1] if "--out" in args else "summary.csv"
    dirs = [a for a in args if a != "--out" and a != out]
    rows = []
    for dd in dirs:
        stage = os.path.basename(os.path.normpath(dd))
        for path in sorted(glob.glob(os.path.join(dd, "*.json"))):
            if path.endswith(".partial") or "_vs_" in path or os.path.basename(path).startswith(("cmp_", "acc_")):
                continue
            try:
                rows.append(row_of(path, stage))
            except Exception as ex:  # noqa: BLE001 - report and continue
                print("skip", path, ex)
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)
    print(f"{len(rows)} runs -> {out}")


if __name__ == "__main__":
    main()
