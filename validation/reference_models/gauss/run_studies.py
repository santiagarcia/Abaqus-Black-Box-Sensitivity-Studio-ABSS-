"""Gauss numerical studies for the ABSS history-consistent residual recursion.

Usage:  python run_studies.py [S0 S1 ...]      (default: all studies)
Each study prints to stdout and to outputs/<study>.txt next to this file.
Requires: numpy, mpmath.  Deterministic (fixed seeds, no wall-clock data in
numerical tables).
"""

from __future__ import annotations

import io
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from abss_gauss_ref import (  # noqa: E402
    MODELS, ElastoplasticBar, cond2, f32, full_rerun_derivs, history_rel_error, hp_reference,
    hp_reference_mixed, newton_solve, oracle_residual, params_at, recursion, scale_coef,
    submatrix, u_scale,
)

HS = [10.0 ** -e for e in range(1, 11)]
OUT = os.path.join(HERE, "outputs")
os.makedirs(OUT, exist_ok=True)
TIGHT = 1e-12  # nominal Newton tol relative to model.fscale

_buf = None


def out(*a):
    s = " ".join(str(x) for x in a)
    print(s)
    if _buf is not None:
        _buf.write(s + "\n")


def e(x):
    return f"{x:9.2e}" if x == x else "      nan"


_REF_CACHE = {}


def ref_for(m, p, K=3, P0=None):
    P0 = P0 or m.P0
    key = (m.name, tuple(sorted(P0.items())), p, K)
    if key not in _REF_CACHE:
        d = {p: abs(P0[p])}
        if isinstance(m, MODELS["A"]):
            nom = newton_solve(m, P0, TIGHT * m.fscale)
            ref = {k: [[0.0] * m.ndof] + [[float(m.bc(t, params_at(P0, d, 1.0))[0] - m.bc(t, P0)[0]) if k == 1 else 0.0]
                                          + [x * abs(P0[p]) ** k for x in m.analytic(t, p, k)]
                                          for t in nom["times"][1:]] for k in range(1, K + 1)}
        else:
            ref = hp_reference(m, P0, d, K)
        _REF_CACHE[key] = ref
    return _REF_CACHE[key]


def slope(vals, hs):
    s = []
    for a, b, ha, hb in zip(vals, vals[1:], hs, hs[1:]):
        s.append(math.log10(a / b) / math.log10(ha / hb) if a > 0 and b > 0 else float("nan"))
    return s


# ---------------------------------------------------------------------------
def S0():
    """Tangent gate: K_i v vs frozen-history oracle directional derivative; cond(K)."""
    for key in "ABC":
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        worst, cmax = 0.0, 0.0
        ts = nom["times"]
        for i in range(1, len(ts)):
            K = nom["Ks"][i]
            KFF = submatrix(K, m.free, m.free)
            cmax = max(cmax, cond2(KFF))
            for col, j in enumerate(m.free):
                eps = 1e-7 * max(1e-3, max(abs(x) for x in nom["us"][i]))
                up, um = list(nom["us"][i]), list(nom["us"][i])
                up[j] += eps
                um[j] -= eps
                rp, _ = oracle_residual(m, ts[i], ts[i - 1], nom["us"][i - 1], up, nom["qs"][i - 1], m.P0)
                rm, _ = oracle_residual(m, ts[i], ts[i - 1], nom["us"][i - 1], um, nom["qs"][i - 1], m.P0)
                fd = [-(a - b) / (2 * eps) for a, b in zip(rp, rm)]
                colK = [K[r][j] for r in range(m.ndof)]
                sc = max(abs(x) for x in colK)
                worst = max(worst, max(abs(a - b) for a, b in zip(fd, colK)) / sc)
        out(f"{m.name:26s} max_i rel|K e_j - FD_frozen| = {worst:.2e}   max_i cond2(K_FF) = {cmax:.3e}   "
            f"Newton iters/inc (first 8) = {nom['iters'][1:9]}")


def S1_S3(keys="ABC"):
    """Main sweeps: recursion (central, K=1,2,3) vs reference and full-rerun FD."""
    for key in keys:
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        out(f"\n=== Model {m.name}: N={m.nsteps} increments, free dofs {m.free}, params {m.P0}")
        out("reference:", "closed-form analytic" if key == "A" else "mpmath 80-digit full reruns, central FD h=1e-12")
        for p in m.P0:
            d = {p: abs(m.P0[p])}
            ref = ref_for(m, p)
            out(f"\n-- parameter {p} (nominal {m.P0[p]}); errors = max over ALL increments & free dofs, relative "
                f"to max|ref| (abs/|u|max if ref==0)")
            out("   h_rel   | rec e1(K=1) e2(K=2)   e3(K=3)  | full-FD e1   e2        e3       |"
                " predres K=1  K=2      K=3  (/fscale) | asym1")
            pr = {1: [], 2: [], 3: []}
            for h in HS:
                r1 = recursion(m, m.P0, d, h, 1, nominal=nom)
                r2 = recursion(m, m.P0, d, h, 2, nominal=nom)
                r3 = recursion(m, m.P0, d, h, 3, nominal=nom)
                er = [history_rel_error(m, r1["coef"][1], ref[1], uscale=us),
                      history_rel_error(m, r2["coef"][2], ref[2], uscale=us),
                      history_rel_error(m, r3["coef"][3], ref[3], uscale=us)]
                fd = full_rerun_derivs(m, m.P0, d, h, 3, TIGHT * m.fscale)
                ef = [history_rel_error(m, fd[k], ref[k], uscale=us) for k in (1, 2, 3)]
                prs = [max(r["pred_res"]) / m.fscale for r in (r1, r2, r3)]
                for k in (1, 2, 3):
                    pr[k].append(prs[k - 1])
                out(f"  {h:8.0e} | {e(er[0])} {e(er[1])} {e(er[2])} | {e(ef[0])} {e(ef[1])} {e(ef[2])} |"
                    f" {e(prs[0])} {e(prs[1])} {e(prs[2])} | {e(max(r1['asym'][1:]))}")
            for k in (1, 2, 3):
                sl = slope(pr[k], HS)
                out(f"   predictor-residual log-slope K={k} (decade by decade, h=1e-1..): "
                    + " ".join(f"{x:5.2f}" for x in sl))


def S4():
    """Non-smooth yield activation near the nominal parameter."""
    for label, sigy in (("kink at distance 1e-5 (sigy=250.0025)", 250.0025), ("kink AT nominal (sigy=250)", 250.0)):
        m = ElastoplasticBar(sigy=sigy)
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        out(f"\n=== {label}; at sigy=250 elem2 trial stress hits yield exactly at increment 8 (F=200, A2=0.8)")
        for p in ("sigy", "E"):
            d = {p: abs(m.P0[p])}
            ref = ref_for(m, p, K=2, P0=m.P0)
            out(f"-- parameter {p}:  [spec probe, central K=2: e1 e2 maxasym1 maxasymC max pred_rel flagged-incs]"
                f"  [seed=prev + order iterations (<=8, tol 1e-8), K=2: e1 e2 maxasymC flagged(asymC>1e-2 or not converged) mean-iters]  [full-FD e1 e2]")
            for h in HS:
                r = recursion(m, m.P0, d, h, 2, nominal=nom)
                rd = recursion(m, m.P0, d, h, 2, nominal=nom, seed="prev", order_iters=8, iter_tol=1e-8)
                fd = full_rerun_derivs(m, m.P0, d, h, 2, TIGHT * m.fscale)
                er = [history_rel_error(m, r["coef"][k], ref[k], uscale=us) for k in (1, 2)]
                ed = history_rel_error(m, rd["coef"][1], ref[1], uscale=us)
                ed2 = history_rel_error(m, rd["coef"][2], ref[2], uscale=us)
                nit = sum(rd["iters"][1][1:]) / m.nsteps
                ef = [history_rel_error(m, fd[k], ref[k], uscale=us) for k in (1, 2)]
                bad = [i for i, (a, b) in enumerate(zip(r["asym"], r["asym_commit"])) if max(a, b) > 1e-2]
                badd = [i for i, (a, dl) in enumerate(zip(rd["asym_commit"], rd["last_delta"][1])) if a > 1e-2 or dl > 1e-8]
                out(f"   {h:8.0e} | {e(er[0])} {e(er[1])} {e(max(r['asym'][1:]))} {e(max(r['asym_commit'][1:]))}"
                    f" {e(max(r['pred_rel'][1:]))} {str(bad[:6]):18s} | {e(ed)} {e(ed2)} {e(max(rd['asym_commit'][1:]))} {str(badd[:6]):14s} {nit:4.1f}"
                    f" | {e(ef[0])} {e(ef[1])}")
        # per-increment profile for sigy direction
        d = {"sigy": abs(m.P0["sigy"])}
        ref = ref_for(m, "sigy", K=2, P0=m.P0)
        for h in (1e-3, 1e-7):
            r = recursion(m, m.P0, d, h, 2, nominal=nom)
            out(f"   per-increment (sigy, h={h:g}): i, |e1|/max|ref1|, asym1")
            den = max(abs(ref[1][i][j]) for i in range(1, len(ref[1])) for j in m.free)
            row = []
            for i in range(1, m.nsteps + 1):
                ei = max(abs(r["coef"][1][i][j] - ref[1][i][j]) for j in m.free) / den
                row.append(f"{i}:{ei:.1e}/{r['asym'][i]:.0e}")
            out("     " + "  ".join(row))


def S5():
    """Stencil comparison (offsets/branches), orders 1-3."""
    from abss_gauss_ref import SCHEMES
    cases = [("B", "k1"), ("B", "tau"), ("C", "E"), ("C", "H")]
    for key, p in cases:
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p)
        out(f"\n=== {m.name} / {p}: best error over h sweep (argmin h) and error at h=1e-3, 1e-5")
        for sch in ("central", "central4", "forward", "forward_baked", "central_baked"):
            Kmax = max(SCHEMES[sch])
            Kmax = min(Kmax, 3)
            line = f"  {sch:14s}"
            for k in range(1, Kmax + 1):
                errs = []
                runs = None
                for h in HS:
                    r = recursion(m, m.P0, d, h, k, sch, nominal=nom)
                    errs.append(history_rel_error(m, r["coef"][k], ref[k], uscale=us))
                    runs = r["runs"] / m.nsteps
                j = min(range(len(HS)), key=lambda q: errs[q])
                line += (f" | k={k}: min {errs[j]:.1e}@{HS[j]:.0e} e(1e-3)={errs[2]:.1e} e(1e-5)={errs[4]:.1e}"
                         f" runs/inc={runs:.0f} br={len(r['branches'])}")
            out(line)


def S6():
    """History ablation: own branch per offset vs frozen nominal history vs one shared branch."""
    cases = [("B", "tau"), ("B", "k1"), ("B", "U0"), ("C", "E"), ("C", "sigy"), ("C", "A2")]
    out("history_mode: branch = each stencil offset m*h carries its own complete history (proposed);")
    out("              frozen = probes use the nominal history (no history propagation);")
    out("              shared = all offsets use the +h (largest offset) branch history")
    for key, p in cases:
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p)
        line = f"  {m.name:26s} {p:5s}"
        for hm in ("branch", "frozen", "shared"):
            r = recursion(m, m.P0, d, 1e-4, 2, nominal=nom, history_mode=hm)
            e1 = history_rel_error(m, r["coef"][1], ref[1], uscale=us)
            e2 = history_rel_error(m, r["coef"][2], ref[2], uscale=us)
            line += f" | {hm}: e1={e1:.1e} e2={e2:.1e}"
        out(line + "   (h=1e-4, central K=2)")


def S7():
    """Output precision: ODB float32 vs double."""
    configs = [
        ("u64 res64 g0=replay", dict(u_read32=False, rf="res64", g0_mode="replay")),
        ("u64 res32 g0=replay", dict(u_read32=False, rf="res32", g0_mode="replay")),
        ("u64 fint32 g0=replay", dict(u_read32=False, rf="fint32", g0_mode="replay")),
        ("u32 res32 g0=replay", dict(u_read32=True, rf="res32", g0_mode="replay")),
        ("u32 res32 g0=newton", dict(u_read32=True, rf="res32", g0_mode="newton")),
        ("u32 fint32 g0=replay", dict(u_read32=True, rf="fint32", g0_mode="replay")),
    ]
    cases = [("B", "k1"), ("B", "beta"), ("C", "E"), ("C", "F0")]
    for key, p in cases:
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p)
        out(f"\n=== {m.name} / {p}: rows h_rel; cols e1 / e2 per precision config (recursion central K=2)")
        out("   h_rel  " + " ".join(f"{c[0]:>22s}" for c in configs) + "   fullFD-u32 e1/e2")
        for h in HS[:9]:
            line = f"  {h:7.0e}"
            for _, kw in configs:
                r = recursion(m, m.P0, d, h, 2, nominal=nom, **kw)
                e1 = history_rel_error(m, r["coef"][1], ref[1], uscale=us)
                e2 = history_rel_error(m, r["coef"][2], ref[2], uscale=us)
                line += f"   {e1:8.1e}/{e2:8.1e}  "
            fd = full_rerun_derivs(m, m.P0, d, h, 2, TIGHT * m.fscale, u_read32=True)
            line += f"   {history_rel_error(m, fd[1], ref[1], uscale=us):8.1e}/{history_rel_error(m, fd[2], ref[2], uscale=us):8.1e}"
            out(line)


def S8():
    """Nominal Newton residual (Abaqus R_n) and the g0 treatment."""
    cases = [("B", "k1"), ("C", "E"), ("C", "F0")]
    for key, p in cases:
        m = MODELS[key]()
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p)
        out(f"\n=== {m.name} / {p}: nominal stopped with ||r_F|| ~ rho*fscale; errors e1/e2 (central K=2) and e1 of the spec one-sided p=[g(h)-g(0)]/h (fwd1) vs exact-equilibrium ref")
        for rho in (5e-3, 1e-5, 1e-8, None):
            nom = newton_solve(m, m.P0, TIGHT * m.fscale, stop_residual=None if rho is None else rho * m.fscale)
            us = u_scale(m, nom)
            line = f"  rho={'tight' if rho is None else f'{rho:.0e}':>6s}"
            for g0 in ("replay", "zero"):
                for h in (1e-2, 1e-4, 1e-6):
                    r = recursion(m, m.P0, d, h, 2, nominal=nom, g0_mode=g0)
                    e1 = history_rel_error(m, r["coef"][1], ref[1], uscale=us)
                    e2 = history_rel_error(m, r["coef"][2], ref[2], uscale=us)
                    rf_ = recursion(m, m.P0, d, h, 1, "forward", nominal=nom, g0_mode=g0)
                    ef = history_rel_error(m, rf_["coef"][1], ref[1], uscale=us)
                    line += f" | {g0[:3]} h={h:.0e}: {e1:.1e}/{e2:.1e} fwd1 {ef:.1e}"
            out(line)


def S9():
    """Cutbacks: perturbed full rerun on a different time grid."""
    for key, p in (("B", "k1"), ("C", "E")):
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p)
        ts = nom["times"]
        grid2 = ts[:5] + [(ts[4] + ts[5]) / 2] + ts[5:]   # one cutback in increment 5
        out(f"\n=== {m.name} / {p}: full-rerun central FD order 1 when the a+h run cut back once (inc 5 split)")
        out("   h_rel  | same grid e1 | +h run cut back e1 | recursion (nominal grid by construction) e1")
        for h in HS[:8]:
            sp = newton_solve(m, params_at(m.P0, d, h), TIGHT * m.fscale, times=grid2)["us"]
            sp = sp[:5] + sp[6:]
            sm = newton_solve(m, params_at(m.P0, d, -h), TIGHT * m.fscale)["us"]
            fd_bad = [[(a - b) / (2 * h) for a, b in zip(x, y)] for x, y in zip(sp, sm)]
            fd = full_rerun_derivs(m, m.P0, d, h, 1, TIGHT * m.fscale)[1]
            r = recursion(m, m.P0, d, h, 1, nominal=nom)
            out(f"  {h:7.0e} | {e(history_rel_error(m, fd, ref[1], uscale=us))}    | {e(history_rel_error(m, fd_bad, ref[1], uscale=us))}"
                f"          | {e(history_rel_error(m, r['coef'][1], ref[1], uscale=us))}")


def S10():
    """Inexact tangent operator and defect correction (order 1)."""
    for key, p in (("B", "k1"), ("B", "tau"), ("C", "E"), ("C", "sigy")):
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p)
        line = f"  {m.name:26s} {p:5s} h=1e-4"
        for ks in (None, 0.02, 0.2):
            for it in (0, 1, 3):
                r = recursion(m, m.P0, d, 1e-4, 1, nominal=nom, K_scale=ks, defect_iters=it)
                line += f" | dK={0 if ks is None else ks:g} it={it}: {history_rel_error(m, r['coef'][1], ref[1], uscale=us):.1e}"
        out(line)


def S11():
    """Mixed derivatives via directional (pure) recursions: polarization."""
    cases = [("B", "k1", "F0"), ("B", "beta", "tau"), ("C", "sigy", "H"), ("C", "E", "H")]
    for key, a, b in cases:
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        da, db = abs(m.P0[a]), abs(m.P0[b])
        r_ab = hp_reference_mixed(m, m.P0, a, b, 1, 1)
        r_aab = hp_reference_mixed(m, m.P0, a, b, 2, 1)
        r_abb = hp_reference_mixed(m, m.P0, a, b, 1, 2)
        us = u_scale(m, nom)
        out(f"\n=== {m.name}: mixed d2u/d{a}d{b}, d3u/d{a}2d{b}, d3u/d{a}d{b}2; ref = 80-digit tensor-product FD of full reruns")
        out("    errors in relative-parameter units (derivative * |a|^ka |b|^kb), relative to max|ref| (abs/|u|max if ref==0)")
        out("   h_rel  | ab: polar(a,b,a+b) | ab: (a+b),(a-b) | aab: 4 dirs  | abb: 4 dirs")
        for h in HS[:8]:
            res = {}
            for name, dd in (("a", {a: da}), ("b", {b: db}), ("p", {a: da, b: db}), ("m", {a: da, b: -db})):
                res[name] = recursion(m, m.P0, dd, h, 3, nominal=nom)["coef"]
            n = len(nom["times"])

            def comb(f):
                return [[f(i, j) for j in range(m.ndof)] for i in range(n)]

            pol = comb(lambda i, j: (res["p"][2][i][j] - res["a"][2][i][j] - res["b"][2][i][j]) / (2 * da * db))
            pm = comb(lambda i, j: (res["p"][2][i][j] - res["m"][2][i][j]) / (4 * da * db))
            aab = comb(lambda i, j: (res["p"][3][i][j] - res["m"][3][i][j] - 2 * res["b"][3][i][j]) / (6 * da * da * db))
            abb = comb(lambda i, j: (res["p"][3][i][j] + res["m"][3][i][j] - 2 * res["a"][3][i][j]) / (6 * da * db * db))
            def er(est, ref, ka, kb):
                s_ = da ** ka * db ** kb
                return history_rel_error(m, scale_coef(est, 1 / s_), scale_coef(ref, 1 / s_), uscale=us)
            out(f"  {h:7.0e} | {e(er(pol, r_ab, 1, 1))}          | {e(er(pm, r_ab, 1, 1))}       |"
                f" {e(er(aab, r_aab, 2, 1))}    | {e(er(abb, r_abb, 1, 2))}")


def S12():
    """Probe seeding: frozen-current-displacement probe (spec) vs probe shifted by previous-increment coefficient."""
    out("seed=none : order-k probe at u^[<k](t) (spec section 3 item 3 for k=1: current displacement = nominal u*)")
    out("seed=prev : order-k probe at u^[<k](t) + t^k/k! coef_(i-1)^(k); coef_i^(k) = seed + k! K^-1 c_k (same identity)")
    out("prev+iter : as prev, re-probing each order at the updated estimate until rel. correction <= 1e-8 (max 8 extra)")
    for key in "BC":
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        for p in m.P0:
            d = {p: abs(m.P0[p])}
            ref = ref_for(m, p)
            out(f"\n-- {m.name} / {p}:   h_rel | none e1  e2  predres | prev e1  e2  predres | prev+iter e1 e2 predres mean-iters  (central K=2)")
            for h in HS[:7]:
                line = f"   {h:7.0e}"
                for sd, oi in (("none", 0), ("prev", 0), ("prev", 8)):
                    r = recursion(m, m.P0, d, h, 2, nominal=nom, seed=sd, order_iters=oi, iter_tol=1e-8)
                    line += (f" | {e(history_rel_error(m, r['coef'][1], ref[1], uscale=us))}"
                             f" {e(history_rel_error(m, r['coef'][2], ref[2], uscale=us))} {e(max(r['pred_res']) / m.fscale)}")
                    if oi:
                        line += f" {sum(r['iters'][1][1:]) / m.nsteps:4.1f}"
                out(line)


def S13():
    """Constrained dofs: exact perturbed BC inside the probe vs spec item 4 (probe at nominal u_C, K_FC s_C on RHS)."""
    for key, ps in (("A", ("U0", "k1")), ("B", ("U0", "k1", "tau"))):
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        for p in ps:
            d = {p: abs(m.P0[p])}
            ref = ref_for(m, p)
            line = f"  {m.name:26s} {p:4s} order-1 e1 (central):"
            for h in (1e-2, 1e-4, 1e-6):
                ex = recursion(m, m.P0, d, h, 1, nominal=nom, bc_mode="exact")
                kf = recursion(m, m.P0, d, h, 1, nominal=nom, bc_mode="kfc")
                line += (f" | h={h:.0e} exact-BC {history_rel_error(m, ex['coef'][1], ref[1], uscale=us):.1e}"
                         f" K_FC-form {history_rel_error(m, kf['coef'][1], ref[1], uscale=us):.1e}")
            out(line)


def S14():
    """Predictor-residual slope for K=3: limited by the order-1 stencil accuracy (central O(h^2) vs central4 O(h^4))."""
    for key, p in (("B", "k1"), ("B", "tau"), ("C", "E"), ("C", "H")):
        m = MODELS[key]()
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        d = {p: abs(m.P0[p])}
        for sch in ("central", "central4"):
            prs = [max(recursion(m, m.P0, d, h, 3, sch, nominal=nom)["pred_res"]) / m.fscale for h in (1e-1, 1e-2, 1e-3)]
            out(f"  {m.name:26s} {p:4s} {sch:9s} predres(h=1e-1,1e-2,1e-3) = " + " ".join(f"{x:.2e}" for x in prs)
                + "  log-slopes " + " ".join(f"{math.log10(a / b):.2f}" for a, b in zip(prs, prs[1:])))


def S15():
    """Recommended adaptive order-1 probing: spec probe first; iterate (re-probe at u*+t s) only if asymmetry > max(1e-2, 30 h)."""
    cases = [("C", {"sigy": 250.0025}, "E"), ("C", {"sigy": 250.0025}, "sigy"), ("C", {}, "E"), ("C", {}, "sigy"),
             ("C", {}, "F0"), ("C", {}, "A2"), ("B", {}, "k1"), ("B", {}, "tau")]
    out("columns per h: e1 e2 (central K=2) | mean extra order-1 probe sets per increment | max asym1, max pred_rel |"
        " flagged increments (asym1 > max(1e-2,30h), or iteration not converged to 1e-8 within 8, or pred_rel > max(1e-3,100h^2))")
    for key, ov, p in cases:
        m = MODELS[key](**ov)
        nom = newton_solve(m, m.P0, TIGHT * m.fscale)
        us = u_scale(m, nom)
        d = {p: abs(m.P0[p])}
        ref = ref_for(m, p, K=2, P0=m.P0)
        out(f"\n-- {m.name} {ov or ''} / {p}")
        for h in (1e-2, 1e-3, 1e-4, 1e-5, 1e-6):
            thr = max(1e-2, 30 * h)
            r = recursion(m, m.P0, d, h, 2, nominal=nom, order_iters=8, iter_tol=1e-8, adaptive_thr=thr)
            e1 = history_rel_error(m, r["coef"][1], ref[1], uscale=us)
            e2 = history_rel_error(m, r["coef"][2], ref[2], uscale=us)
            extra = sum(r["iters"][1][1:]) / m.nsteps
            pthr = max(1e-3, 100 * h * h)
            flagged = [i for i in range(1, m.nsteps + 1)
                       if r["asym"][i] > thr or (r["iters"][1][i] > 0 and r["last_delta"][1][i] > 1e-8)
                       or r["pred_rel"][i] > pthr]
            out(f"   h={h:7.0e} | {e(e1)} {e(e2)} | extra {extra:4.2f} | max asym {e(max(r['asym'][1:]))}"
                f" max pred_rel {e(max(r['pred_rel'][1:]))} | flagged {flagged}")


STUDIES = {"S0": S0, "S1": lambda: S1_S3("A"), "S2": lambda: S1_S3("B"), "S3": lambda: S1_S3("C"), "S4": S4,
           "S5": S5, "S6": S6, "S7": S7, "S8": S8, "S9": S9, "S10": S10, "S11": S11, "S12": S12, "S13": S13, "S14": S14, "S15": S15}

if __name__ == "__main__":
    import numpy
    import mpmath
    names = sys.argv[1:] or list(STUDIES)
    for nm in names:
        _buf = io.StringIO()
        out(f"##### {nm}: {(STUDIES[nm].__doc__ or {'S1': 'model A sweep', 'S2': 'model B sweep', 'S3': 'model C sweep'}[nm]).strip()}")
        out(f"python {sys.version.split()[0]}, numpy {numpy.__version__}, mpmath {mpmath.__version__}")
        STUDIES[nm]()
        with open(os.path.join(OUT, f"{nm}.txt"), "w", encoding="utf-8") as fh:
            fh.write(_buf.getvalue())
        _buf = None
