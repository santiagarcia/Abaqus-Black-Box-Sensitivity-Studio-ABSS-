"""Noether reference check of the ABSS history-consistent sensitivity recursion.

Model: 2-DOF incremental problem with one free DOF ``u`` and one prescribed DOF
``v = g(a, t)``, a history variable ``q`` and a scalar parameter ``a`` entering
the load, the internal force, the prescribed value and the history update.

    r(u, v, u_prev, q_prev, a, t) = t (1 + 0.2 a^2)
        - [a u + 0.4 u^3 + 2 (u - v) + q_prev u + 0.5 a (u - u_prev)^2]
    Q(u, v, u_prev, q_prev, a) = q_prev + 0.3 a (u - u_prev)^2 + 0.1 (u - u_prev) v
    g(a, t) = t (0.1 + 0.05 a^2)
    S(u, v, u_prev, q_prev, a) = a u - 0.5 q_prev + (u - v)^2   (an output "field")

K = -dr/du (positive structural sign), K_FC = -dr/dv.

Checks
  A. Exact [h^k] recursion (sympy truncated series) vs independent reference
     derivatives of complete nonlinear re-solves (mpmath, 60 digits, mp.diff).
     Form B (prescribed DOF at g(a+h)) and Form A (frozen prescribed DOF + K_FC term).
  B. Counterexample: history advanced with a lower-order predictor.
  C. Output-field derivatives from the order-K commit branch.
  D. Finite-offset lockstep algorithm with polynomial fit: error vs h.
  E. Re-equilibrated (tolerance-limited) history branch vs predictor branch.
  F. Effect of float32-like rounding (a) of the reported residual and (b) of the
     prescribed displacements written to the probe/commit inputs.
  G. Approximate tangent with oracle-only defect correction (first order).

Run:  python check_recursion.py   (needs sympy + mpmath; ~1-3 min)
"""

from __future__ import annotations

import math

import mpmath as mp
import sympy as sp

mp.mp.dps = 60
KMAX = 3
A0_STR = "1.3"
TIMES_STR = ["0.25", "0.5", "0.75", "1.0"]


def make_model(c):
    """Return model functions using the constant constructor ``c``."""

    def r(u, v, up, qp, a, t):
        return t * (1 + c("0.2") * a**2) - (
            a * u + c("0.4") * u**3 + 2 * (u - v) + qp * u + c("0.5") * a * (u - up) ** 2
        )

    def Q(u, v, up, qp, a):
        return qp + c("0.3") * a * (u - up) ** 2 + c("0.1") * (u - up) * v

    def g(a, t):
        return t * (c("0.1") + c("0.05") * a**2)

    def S(u, v, up, qp, a):
        return a * u - c("0.5") * qp + (u - v) ** 2

    def K(u, v, up, qp, a):  # -dr/du
        return a + c("1.2") * u**2 + 2 + qp + a * (u - up)

    def KFC(u, v, up, qp, a):  # -dr/dv
        return -2 + 0 * u

    return r, Q, g, S, K, KFC


mr, mQ, mg, mS, mK, mKFC = make_model(mp.mpf)
sr, sQ, sg, sS, sK, sKFC = make_model(sp.Rational)
A0 = mp.mpf(A0_STR)
TIMES = [mp.mpf(s) for s in TIMES_STR]


# ---------------------------------------------------------------- reference
def march(a, tol=None, guess=None):
    """Complete nonlinear re-solve. Returns lists u, v, q, S per increment."""
    up, qp = mp.mpf(0), mp.mpf(0)
    us, vs, qs, Ss = [], [], [], []
    for i, t in enumerate(TIMES):
        v = mg(a, t)
        f = lambda u: mr(u, v, up, qp, a, t)  # noqa: E731
        x0 = up if guess is None else guess[i]
        if tol is None:
            u = mp.findroot(f, x0)
        else:  # Newton stopped at |r| < tol (mimics a convergence tolerance)
            u = x0
            while abs(f(u)) >= tol:
                u = u + f(u) / mK(u, v, up, qp, a)
        Ss.append(mS(u, v, up, qp, a))
        q = mQ(u, v, up, qp, a)
        us.append(u), vs.append(v), qs.append(q)
        up, qp = u, q
    return us, vs, qs, Ss


NOM_U, NOM_V, NOM_Q, NOM_S = march(A0)
N = len(TIMES)
REF_U = [[mp.diff(lambda a, i=i: march(a)[0][i], A0, k) for k in range(KMAX + 1)] for i in range(N)]
REF_S = [[mp.diff(lambda a, i=i: march(a)[3][i], A0, k) for k in range(KMAX + 1)] for i in range(N)]


def nominal_state(i):
    up = NOM_U[i - 1] if i > 0 else mp.mpf(0)
    qp = NOM_Q[i - 1] if i > 0 else mp.mpf(0)
    return NOM_U[i], NOM_V[i], up, qp


# ---------------------------------------------------------------- A/B/C: exact series recursion
h = sp.Symbol("h")


def trunc(expr, order=KMAX):
    p = sp.Poly(sp.expand(expr), h)
    return sum(cf * h**m for (m,), cf in p.terms() if m <= order)


def coeff(expr, m):
    return sp.Poly(sp.expand(expr), h).coeff_monomial(h**m)


def F(x):
    return sp.Float(mp.nstr(x, 60), 60)


def series_recursion(form="B", hist_order=KMAX):
    """Return U[i][m] (m-th derivative) and Sout[i][m] via the probe recursion."""
    a = F(A0) + h
    up, qp = sp.Integer(0), sp.Integer(0)
    U, Sout, lowdiag = [], [], []
    for i, ts in enumerate(TIMES_STR):
        t = sp.Rational(ts)
        u0, v0, up0, qp0 = nominal_state(i)
        Kt = F(mK(u0, v0, up0, qp0, A0))
        Kfc = F(mKFC(u0, v0, up0, qp0, A0))
        gser = trunc(sg(a, t))
        Ui = [F(u0)]
        for m in range(1, KMAX + 1):
            pred = sum(h**j / math.factorial(j) * Ui[j] for j in range(m))
            if form == "B":
                v = gser
                rhs = math.factorial(m) * coeff(trunc(sr(pred, v, up, qp, a, t)), m)
            else:  # form A: prescribed DOF held at its (m-1)-order Taylor polynomial
                v = trunc(gser, m - 1)
                gm = math.factorial(m) * coeff(gser, m)
                rhs = math.factorial(m) * coeff(trunc(sr(pred, v, up, qp, a, t)), m) - Kfc * gm
            res = trunc(sr(pred, gser if form == "B" else v, up, qp, a, t))
            lowdiag.append(max([abs(coeff(res, j)) for j in range(1, m)] + [0]))
            Ui.append(rhs / Kt)
        full = sum(h**j / math.factorial(j) * Ui[j] for j in range(hist_order + 1))
        Sser = trunc(sS(full, gser, up, qp, a))
        Sout.append([math.factorial(m) * coeff(Sser, m) for m in range(KMAX + 1)])
        qn = trunc(sQ(full, gser, up, qp, a))
        up, qp = full, qn
        U.append(Ui)
    return U, Sout, max(lowdiag)


def rel(x, y):
    x, y = mp.mpf(str(x)), mp.mpf(str(y))
    return abs(x - y) / max(abs(y), mp.mpf("1e-30"))


def report_A():
    print("=== A. exact [h^k] recursion vs complete-re-solve reference (rel. error) ===")
    for form in ("B", "A"):
        U, Sout, lowdiag = series_recursion(form=form)
        worst = [max(rel(U[i][k], REF_U[i][k]) for i in range(N)) for k in range(1, KMAX + 1)]
        print(f" form {form}: max rel err d^k u/da^k, k=1..{KMAX}:", [mp.nstr(w, 3) for w in worst],
              " max |lower-order probe coeff|:", mp.nstr(mp.mpf(str(lowdiag)), 3))
        if form == "B":
            Sw = [max(rel(Sout[i][k], REF_S[i][k]) for i in range(N)) for k in range(1, KMAX + 1)]
            print(f" C. output S from order-{KMAX} commit branch, max rel err k=1..{KMAX}:",
                  [mp.nstr(w, 3) for w in Sw])
    print(" reference values at last increment: d^k u/da^k =",
          [mp.nstr(REF_U[N - 1][k], 12) for k in range(KMAX + 1)])
    print("=== B. counterexample: history committed with order-L predictor (form B) ===")
    for L in (0, 1, 2):
        U, _, _ = series_recursion(form="B", hist_order=L)
        worst = [max(rel(U[i][k], REF_U[i][k]) for i in range(N)) for k in range(1, KMAX + 1)]
        print(f" L={L}: max rel err k=1..{KMAX}:", [mp.nstr(w, 3) for w in worst])


# ---------------------------------------------------------------- D/E/F: finite offsets
def vandermonde_coeffs(nodes, ys):
    n = len(nodes)
    V = mp.matrix(n, n)
    for p, x in enumerate(nodes):
        for j in range(n):
            V[p, j] = x**j
    return mp.lu_solve(V, mp.matrix(ys))


def lockstep(offsets, K, noise=0, hist_order=None, udigits=0):
    """Implementer algorithm: offset branches in lockstep, polynomial fit per order.

    noise:   round each reported residual to this many significant digits.
    udigits: round every prescribed displacement written to a probe/commit
             (i.e. to the INP) to this many significant digits.
    """
    hist_order = K if hist_order is None else hist_order
    rnd = (lambda x: mp.mpf(mp.nstr(x, udigits))) if udigits else (lambda x: x)  # noqa: E731
    nodes = [mp.mpf(0)] + [mp.mpf(o) for o in offsets]
    br = [[mp.mpf(0), mp.mpf(0)] for _ in nodes]  # (u_prev, q_prev) per branch; node 0 = nominal replay
    out = []
    for i, t in enumerate(TIMES):
        u0, v0, up0, qp0 = nominal_state(i)
        Kt = mK(u0, v0, up0, qp0, A0)
        Ui = [u0]
        for m in range(1, K + 1):
            ys = []
            for p, hl in enumerate(nodes):
                a = A0 + hl
                pred = rnd(sum(hl**j / math.factorial(j) * Ui[j] for j in range(m)))
                y = mr(pred, mg(a, t), br[p][0], br[p][1], a, t)
                if noise:  # relative rounding of the reported residual (e.g. float32 ODB)
                    y = mp.mpf(float(mp.nstr(y, noise)))
                ys.append(y)
            c = vandermonde_coeffs(nodes, ys)
            Ui.append(math.factorial(m) * c[m] / Kt)
        for p, hl in enumerate(nodes):
            a = A0 + hl
            full = rnd(sum(hl**j / math.factorial(j) * Ui[j] for j in range(hist_order + 1)))
            q = mQ(full, mg(a, t), br[p][0], br[p][1], a)
            br[p] = [full, q]
        out.append(Ui)
    return out


def err_vs_ref(out, k):
    return max(rel(out[i][k], REF_U[i][k]) for i in range(N))


def report_D():
    print("=== D. finite-offset lockstep + polynomial fit, max rel err over increments ===")
    for hs in ("1e-2", "1e-3", "1e-4"):
        hh = mp.mpf(hs)
        fwd = lockstep([hh], 1)
        cen = lockstep([hh, -hh], 2)
        c5 = lockstep([hh, -hh, 2 * hh, -2 * hh], 3)
        print(f" h={hs}: k=1 fwd {mp.nstr(err_vs_ref(fwd, 1), 3)} | k=1 cen {mp.nstr(err_vs_ref(cen, 1), 3)}"
              f" k=2 cen {mp.nstr(err_vs_ref(cen, 2), 3)} | 5-node k=1 {mp.nstr(err_vs_ref(c5, 1), 3)}"
              f" k=2 {mp.nstr(err_vs_ref(c5, 2), 3)} k=3 {mp.nstr(err_vs_ref(c5, 3), 3)}")


def reequilibrated_first_order(hh, tol):
    """First order with history from a Newton re-solve at a+h stopped at |r|<tol."""
    a = A0 + hh
    ub, vb, qb, _ = march(a, tol=tol, guess=NOM_U)
    s = []
    for i, t in enumerate(TIMES):
        u0, v0, up0, qp0 = nominal_state(i)
        upb = ub[i - 1] if i else mp.mpf(0)
        qpb = qb[i - 1] if i else mp.mpf(0)
        y1 = mr(u0, mg(a, t), upb, qpb, a, t)  # form B probe
        y0 = mr(u0, v0, up0, qp0, A0, t)  # nominal probe (not assumed zero)
        s.append((y1 - y0) / hh / mK(u0, v0, up0, qp0, A0))
    return max(rel(s[i], REF_U[i][1]) for i in range(N))


def report_E():
    print("=== E. first order, forward FD: predictor branch vs re-equilibrated branch ===")
    for hs in ("1e-2", "1e-3", "1e-4", "1e-5"):
        hh = mp.mpf(hs)
        pred = err_vs_ref(lockstep([hh], 1), 1)
        tight = reequilibrated_first_order(hh, mp.mpf("1e-40"))
        loose = reequilibrated_first_order(hh, mp.mpf("1e-6"))
        print(f" h={hs}: predictor {mp.nstr(pred, 3)} | re-equil tol=1e-40 {mp.nstr(tight, 3)}"
              f" | re-equil tol=1e-6 {mp.nstr(loose, 3)}")


def report_F():
    print("=== F. float32-like rounding (7 sig. digits), max rel err ===")
    for label, kw in (("reported residual rounded", {"noise": 7}),
                      ("prescribed displacements rounded", {"udigits": 7}),
                      ("prescribed displacements at 17 digits", {"udigits": 17})):
        print(f" -- {label}")
        for hs in ("1e-1", "1e-2", "1e-3", "1e-4", "1e-5"):
            hh = mp.mpf(hs)
            cen = lockstep([hh, -hh], 2, **kw)
            c5 = lockstep([hh, -hh, 2 * hh, -2 * hh], 3, **kw)
            print(f"   h={hs}: k=1 cen {mp.nstr(err_vs_ref(cen, 1), 3)}"
                  f" k=2 cen {mp.nstr(err_vs_ref(cen, 2), 3)} | 5-node k=3 {mp.nstr(err_vs_ref(c5, 3), 3)}")


def lockstep_refined(hh, kfac, iters):
    """First order, centered offsets, with an approximate tangent kfac*K plus
    oracle-only defect correction  s <- s + Kapprox^{-1} [r(+h)-r(-h)]/(2h)
    evaluated at the predictor u* + h s (current increment, same history)."""
    nodes = [hh, -hh]
    br = [[mp.mpf(0), mp.mpf(0)] for _ in nodes]
    out = []
    for i, t in enumerate(TIMES):
        u0, v0, up0, qp0 = nominal_state(i)
        Ka = kfac * mK(u0, v0, up0, qp0, A0)

        def probe(s):
            ys = [mr(u0 + hl * s, mg(A0 + hl, t), br[p][0], br[p][1], A0 + hl, t) for p, hl in enumerate(nodes)]
            return (ys[0] - ys[1]) / (2 * hh)

        s = probe(mp.mpf(0)) / Ka
        for _ in range(iters):
            s = s + probe(s) / Ka
        for p, hl in enumerate(nodes):
            full = u0 + hl * s
            br[p] = [full, mQ(full, mg(A0 + hl, t), br[p][0], br[p][1], A0 + hl)]
        out.append([u0, s])
    return out


def report_G():
    print("=== G. approximate tangent (K scaled by 1.5) + oracle defect correction, h=1e-4 ===")
    for it in (0, 1, 3, 10, 30):
        e = err_vs_ref(lockstep_refined(mp.mpf("1e-4"), mp.mpf("1.5"), it), 1)
        print(f" corrections={it}: max rel err ds/da {mp.nstr(e, 3)}")


if __name__ == "__main__":
    report_A()
    report_D()
    report_E()
    report_F()
    report_G()
