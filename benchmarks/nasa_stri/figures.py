"""Campaign figures (large fonts; PDF + SVG + PNG). Reads summary.csv (analyze.py) and cmp_*.json (compare_all.py).

Usage:  python figures.py <summary.csv> <results root> <figure dir>
Every point is a measurement: medians over repeats with min-max bars; single runs are drawn hollow and labelled.
"""
import csv
import glob
import json
import os
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, NullFormatter  # noqa: E402


def tidy(ax, xs=None):
    """Plain-number tick labels on log axes (slides); x ticks at the measured values when few."""
    plain = FuncFormatter(lambda v, _: f"{v:,.0f}" if v >= 10 else f"{v:g}")
    vals = sorted({float(x) for x in (xs or []) if x == x and float(x) > 0})
    if vals and len(vals) <= 8:
        lo, hi = ax.get_xlim()
        ax.set_xticks([v for v in vals if lo <= v <= hi])
        ax.xaxis.set_major_formatter(plain)
        ax.xaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_major_formatter(plain)
    # label minor ticks whose leading digit is 2, 3 or 5 (e.g. 0.2, 30, 500) so short log ranges stay readable
    ax.yaxis.set_minor_formatter(FuncFormatter(lambda v, _: (f"{v:,.0f}" if v >= 10 else f"{v:g}")
                                               if v > 0 and f"{v:e}"[0] in "235" else ""))

plt.rcParams.update({"font.size": 17, "axes.titlesize": 18, "axes.labelsize": 18, "legend.fontsize": 14,
                     "xtick.labelsize": 15, "ytick.labelsize": 15, "lines.linewidth": 2.5, "lines.markersize": 9,
                     "figure.figsize": (9.5, 6.2), "savefig.bbox": "tight"})
C_ABSS, C_FAFD, C_BATCH = "#1f5fbf", "#c4511a", "#2a8a2a"
METHODS = [("abss", "ABSS (one job per branch)", C_ABSS), ("abss_batch", "ABSS (replicated branches)", C_BATCH),
           ("fafd", "FAFD", C_FAFD)]

summary, root, figdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(figdir, exist_ok=True)
rows = [r for r in csv.DictReader(open(summary))]


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def ok(r):
    return r["failed"] in ("0", "") and num(r["t_sensitivity"]) is not None


def save(fig, name):
    for ext in ("pdf", "svg", "png"):
        fig.savefig(os.path.join(figdir, f"{name}.{ext}"), dpi=200 if ext == "png" else None)
    plt.close(fig)
    print("figure", name)


CASE_FILTER = None  # optional predicate on a row's case id (set around a figure call)


def groups(stage, xkey, method, ykey):
    """{x: [y...]} over successful runs."""
    g = {}
    for r in rows:
        if r["stage"] != stage or r["method"] != method or not ok(r):
            continue
        if CASE_FILTER and not CASE_FILTER(r["case"]):
            continue
        x, y = num(r[xkey]), num(r[ykey])
        if x is None or y is None:
            continue
        g.setdefault(x, []).append(y)
    return dict(sorted(g.items()))


def plot_med(ax, g, color, label, marker):
    xs = list(g)
    if not xs:
        return
    med = [statistics.median(v) for v in g.values()]
    lo = [m - min(v) for m, v in zip(med, g.values())]
    hi = [max(v) - m for m, v in zip(med, g.values())]
    single = [len(v) == 1 for v in g.values()]
    ax.errorbar(xs, med, yerr=[lo, hi], color=color, marker=marker, capsize=5, label=label,
                markerfacecolor="white" if all(single) else color)


def time_vs(stage, xkey, xlabel, name, title):
    fig, ax = plt.subplots()
    drawn = False
    for m, lab, col in METHODS:
        gs, gt = groups(stage, xkey, m, "t_sensitivity"), groups(stage, xkey, m, "t_workflow")
        if not gs:
            continue
        drawn = True
        plot_med(ax, gs, col, f"{lab}: sensitivity", "o")
        plot_med(ax, gt, col, f"{lab}: nominal + sensitivity", "s")
        ax.get_lines()[-1].set_linestyle("--")
    if not drawn:
        plt.close(fig)
        return
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("wall-clock time [s]")
    tidy(ax, [x for line in ax.get_lines() for x in line.get_xdata()])
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=11)
    save(fig, name)

def speedup_vs(stage, xkey, xlabel, name, title):
    fig, ax = plt.subplots()
    any_ = False
    for m, lab, col in METHODS[:2]:
        for ykey, what, mk, ls in (("t_sensitivity", "sensitivity", "o", "-"), ("t_workflow", "nominal + sensitivity", "s", "--")):
            a, f = groups(stage, xkey, m, ykey), groups(stage, xkey, "fafd", ykey)
            xs = [x for x in a if x in f]
            if not xs:
                continue
            any_ = True
            # ratio of medians; bars span the extreme combinations of the repeats
            med = [statistics.median(f[x]) / statistics.median(a[x]) for x in xs]
            lo = [m_ - min(f[x]) / max(a[x]) for m_, x in zip(med, xs)]
            hi = [max(f[x]) / min(a[x]) - m_ for m_, x in zip(med, xs)]
            ax.errorbar(xs, med, yerr=[lo, hi], marker=mk, ls=ls, capsize=5, color=col, label=f"{lab}, {what}")
    if not any_:
        plt.close(fig)
        return
    ax.axhline(1.0, color="k", lw=1.5, ls=":", label="parity: T_FAFD = T_ABSS")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("speedup  T_FAFD / T_ABSS  (>1: ABSS faster)")
    tidy(ax, [x for line in ax.get_lines() for x in line.get_xdata() if x == x])
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=11)
    save(fig, name)

def jobs_vs(stage, xkey, xlabel, name, title):
    fig, ax = plt.subplots()
    for m, lab, c in METHODS:
        g = groups(stage, xkey, m, "abaqus_jobs")
        if g:
            ax.plot(list(g), [statistics.median(v) for v in g.values()], "o-", color=c, label=f"{lab}: Abaqus jobs")
    g = groups(stage, xkey, "abss", "gmres_matvecs")
    if g and any(statistics.median(v) for v in g.values()):
        ax.plot(list(g), [statistics.median(v) for v in g.values()], "^--", color="#2a8a2a", label="ABSS GMRES matvecs")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("count per run")
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    save(fig, name)


def breakdown(stage, name, title):
    cats = [("job_driver_licence_s", "driver + licence"), ("job_pre_s", "input processing"), ("job_standard_s", "Abaqus/Standard"),
            ("job_wrapup_s", "wrap-up"), ("job_parse_s", "result parsing"), ("engine_factorize_s", "ABSS factorization"),
            ("engine_solve_s", "ABSS solves"), ("engine_tangent_assembly_s", "ABSS tangent assembly")]
    sel = [r for r in rows if r["stage"] == stage and ok(r) and r["repeat"] in ("1", 1)]
    if not sel:
        return
    sizes = sorted({num(r["free_dofs"]) for r in sel})
    pick = [sizes[0], sizes[-1]] if len(sizes) > 1 else sizes
    labels, data = [], []
    for s in pick:
        for m, short in (("abss", "ABSS"), ("abss_batch", "ABSS-R"), ("fafd", "FAFD")):
            r = next((r for r in sel if num(r["free_dofs"]) == s and r["method"] == m), None)
            if r:
                labels.append(f"{short}\n{int(s):,}")
                data.append([num(r[k]) or 0.0 for k, _ in cats])
    fig, ax = plt.subplots(figsize=(11, 6.5))
    bottom = [0.0] * len(data)
    cmap = plt.get_cmap("tab10")
    for j, (_, lab) in enumerate(cats):
        vals = [d[j] for d in data]
        if not any(vals):
            continue
        ax.bar(labels, vals, bottom=bottom, label=lab, color=cmap(j))
        bottom = [b + v for b, v in zip(bottom, vals)]
    ax.set_ylabel("summed worker time [s]")
    ax.set_title(title)
    ax.legend(fontsize=12, ncol=2)
    save(fig, name)


# Annotations backed by the run logs (engine diagnostics), keyed by figure name.
NOTES = {"B_accuracy_vs_dofs": [(7007, "one derivative ABSS flagged non-smooth\n(segment 3 yield onset inside the\nFD stencil at increment 2)")]}


def accuracy_vs_mesh(stage, name, title):
    # Near-zero-safe metric (accuracy.py): max|ABSS - FAFD| in units of the derivative's natural size max|F|/|a|,
    # worst over all parameters of the run, per field.
    pts = {}  # (method, field) -> {free DOFs: [errors]}
    for path in glob.glob(os.path.join(root, stage, "acc_*__*.json")):
        case, rest = os.path.basename(path)[4:-5].split("__")
        method = rest.rsplit("_j", 1)[0]
        r = next((r for r in rows if r["stage"] == stage and r["case"] == case and r["method"] == method), None)
        if not r:
            continue
        worst = {}
        for e in json.load(open(path))["rows"]:
            # fields that are identically zero in this model (e.g. PEEQ of an elastic body) carry no information
            if e.get("err_natural") is not None and e.get("nominal_field_max", 0) > 0:
                worst[e["field"]] = max(worst.get(e["field"], 0.0), e["err_natural"])
        for fld, v in worst.items():  # both ABSS variants pooled: worst of the two
            pts.setdefault(("abss", fld), {}).setdefault(num(r["free_dofs"]), []).append(v)
    if not pts:
        return
    fig, ax = plt.subplots()
    marks = {"U": "o", "RF": "s", "S": "^", "E": "v", "PEEQ": "D", "SDV": "P", "S_Mises": "x"}
    for (m, fld), g in sorted(pts.items()):
        if fld not in marks:
            continue
        xs = sorted(g)
        ax.plot(xs, [max(max(g[x]), 1e-17) for x in xs], marker=marks[fld], ls="-", label=fld)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("free DOFs")
    ax.set_ylabel("max |ABSS - FAFD| / (max|F| / |a|)")
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.axhline(1e-3, color="k", ls=":", lw=1.5, label="validation tolerance 1e-3")
    for x, text in NOTES.get(name, []):
        ax.annotate(text, xy=(x, 7e-4), xytext=(x * 0.13, 2.5e-5), fontsize=11,
                    arrowprops={"arrowstyle": "->", "color": "0.3"}, color="0.2")
    ax.legend(ncol=2, fontsize=12, title="worst over parameters, repeats and both ABSS variants", title_fontsize=11)
    save(fig, name)


time_vs("A_mesh_j8", "free_dofs", "free DOFs", "A_time_vs_dofs_j8", "Family A (elastic), P = 4, 8 job slots each")
speedup_vs("A_mesh_j8", "free_dofs", "free DOFs", "A_speedup_vs_dofs_j8", "Family A, P = 4, matched 8 slots")
time_vs("A_mesh_j1", "free_dofs", "free DOFs", "A_time_vs_dofs_j1", "Family A, P = 4, serial (1 slot each)")
speedup_vs("A_mesh_j1", "free_dofs", "free DOFs", "A_speedup_vs_dofs_j1", "Family A, P = 4, serial")
time_vs("A_params_j8", "n_params", "parameters P", "A_time_vs_params_j8", "Family A, 15,552 free DOFs, 8 slots")
speedup_vs("A_params_j8", "n_params", "parameters P", "A_speedup_vs_params_j8", "Family A, 15,552 free DOFs, 8 slots")
time_vs("A_inc_j8", "increments", "increments", "A_time_vs_increments_j8", "Family A, 2,400 free DOFs, P = 4, 8 slots")
jobs_vs("A_mesh_j8", "free_dofs", "free DOFs", "A_jobs_vs_dofs", "Family A: Abaqus jobs per run (P = 4)")
jobs_vs("A_params_j8", "n_params", "parameters P", "A_jobs_vs_params", "Family A: Abaqus jobs per run")
breakdown("A_mesh_j8", "A_breakdown_j8", "Where the time goes (family A, P = 4, repeat 1)")
accuracy_vs_mesh("A_mesh_j8", "A_accuracy_vs_dofs", "ABSS vs FAFD agreement (family A)")
time_vs("B_mesh_j8", "free_dofs", "free DOFs", "B_time_vs_dofs_j8", "Family B (J2 load/unload), P = 4, 8 slots")
speedup_vs("B_mesh_j8", "free_dofs", "free DOFs", "B_speedup_vs_dofs_j8", "Family B, P = 4, matched 8 slots")
jobs_vs("B_mesh_j8", "free_dofs", "free DOFs", "B_jobs_vs_dofs", "Family B: jobs and GMRES work")
accuracy_vs_mesh("B_mesh_j8", "B_accuracy_vs_dofs", "ABSS vs FAFD agreement (family B)")

# Serial (1 slot each) crossover bracketing: parameter count at 432 free DOFs, mesh size at P = 4
CASE_FILTER = lambda c: c.startswith("A16x2x2")
time_vs("serial_crossover", "n_params", "parameters P", "S_time_vs_params_j1", "Family A, 432 free DOFs, 1 slot each (3 repeats)")
speedup_vs("serial_crossover", "n_params", "parameters P", "S_speedup_vs_params_j1", "Family A, 432 free DOFs, matched 1 slot")
CASE_FILTER = lambda c: c.endswith("_P4")
time_vs("serial_crossover", "free_dofs", "free DOFs", "S_time_vs_dofs_j1", "Family A, P = 4, 1 slot each (3 repeats)")
speedup_vs("serial_crossover", "free_dofs", "free DOFs", "S_speedup_vs_dofs_j1", "Family A, P = 4, matched 1 slot")
CASE_FILTER = None


def before_after(name, title):
    sel = [r for r in rows if r["stage"] == "before_after" and ok(r)]
    if not sel:
        return
    cases = []
    for r in sel:
        k = (r["case"], r["jobs"])
        if k not in cases:
            cases.append(k)
    order = [("abss_before", "ABSS before (original schedule)", "#999999"), ("abss", "ABSS now, one job per branch", C_ABSS),
             ("abss_batch", "ABSS now, replicated branches", C_BATCH), ("fafd", "FAFD (baseline)", C_FAFD)]
    fig, ax = plt.subplots(figsize=(12, 6.5))
    w = 0.2
    for k, (m, lab, col) in enumerate(order):
        ys = []
        for c, j in cases:
            r = next((r for r in sel if r["case"] == c and r["jobs"] == j and r["method"] == m), None)
            ys.append(num(r["t_sensitivity"]) if r else 0.0)
        xs = [i + (k - 1.5) * w for i in range(len(cases))]
        ax.bar(xs, ys, w, label=lab, color=col)
        for x, y in zip(xs, ys):
            if y:
                ax.text(x, y * 1.05, f"{y:.0f}", ha="center", va="bottom", fontsize=10, rotation=90)
    ax.set_xticks(range(len(cases)))
    ax.set_xticklabels([f"{c}\n{j} slot{'s' if j != '1' else ''}" for c, j in cases], fontsize=12)
    ax.set_yscale("log")
    ax.set_ylabel("sensitivity stage wall time [s]")
    ax.set_title(title)
    ax.set_ylim(top=ax.get_ylim()[1] * 6)
    ax.legend(fontsize=11, loc="upper right", ncol=2)
    ax.grid(True, axis="y", which="both", alpha=0.3)
    save(fig, name)


before_after("before_after", "Identical cases: before vs after the changes (single runs)")
