"""Gauss reference prototype: history-consistent residual-oracle sensitivities.

Independent numerical reference for ABSS spec section 3 (first order and the
higher-order Taylor-predictor recursion).  Pure Python + numpy (numpy only for
float32 emulation); mpmath for high-precision reference reruns.

Conventions (identical to the spec)
-----------------------------------
* r = f_ext - f_int over ALL dofs (at a prescribed dof r = -RF).
* K = -dr/du = df_int/du  (positive structural stiffness).
* A "branch" is a complete history replay in which EVERY dof is prescribed at
  every accepted increment (no global Newton): the oracle call
      oracle(t_i, t_{i-1}, u_prev, u_cur, q_prev, P) -> (f_int, f_ext, q_new)
  plays the role of one frozen-displacement Abaqus increment with RF output.
  The algorithm never looks inside the oracle; it only calls it, and uses the
  nominal tangent K_i (the analogue of *OPERATOR OUTPUT / mtx).
* Perturbations are directional: P(t) = P0 + t*d.  coef[i][k] is the k-th
  total derivative d^k u_i/dt^k (physical derivative = coef / d_p^k for a pure
  parameter direction d = d_p e_p).

Algorithm (order K, offset set M, step h) per increment i:
  for k = 1..K:
     g_m = r_F( u_i^[<k](m h), history of branch m through i-1, P0 + m h d )
     c_k = sum_m w_{k,m} g_m / h^k              (exact stencil weights)
     K_FF coef_i^(k) = k! c_k                   (constrained dofs: exact BC)
  commit every branch m != 0 at u_i^[<=K](m h) with the perturbed BC.
The m = 0 value g_0 is measured on a nominal *replay* branch (all dofs
prescribed to the nominal u_i* as read), never assumed to be zero.
"""

from __future__ import annotations

import math
from fractions import Fraction

import mpmath
import numpy as np

mpf = mpmath.mpf


# --------------------------------------------------------------------------
# small generic helpers (work for float and mpmath.mpf)
# --------------------------------------------------------------------------

def f32(x):
    """Round a float to IEEE single precision (ODB field output emulation)."""
    return float(np.float32(x))


def _sin(x):
    return mpmath.sin(x) if isinstance(x, mpmath.mpf) else math.sin(x)


def solve_dense(A, b):
    """Gaussian elimination with partial pivoting (generic number type)."""
    n = len(b)
    M = [list(A[r]) + [b[r]] for r in range(n)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        M[c], M[p] = M[p], M[c]
        for r in range(c + 1, n):
            f = M[r][c] / M[c][c]
            for j in range(c, n + 1):
                M[r][j] -= f * M[c][j]
    x = [0] * n
    for r in reversed(range(n)):
        s = M[r][n] - sum(M[r][j] * x[j] for j in range(r + 1, n))
        x[r] = s / M[r][r]
    return x


def submatrix(K, rows, cols):
    return [[K[r][c] for c in cols] for r in rows]


def cond2(A):
    return float(np.linalg.cond(np.array(A, dtype=float)))


# --------------------------------------------------------------------------
# manufactured models (the "opaque oracle" lives here; algorithm never peeks)
# --------------------------------------------------------------------------

class Model:
    name = "base"
    ndof = 0
    con: list[int] = []
    free: list[int] = []
    nsteps = 0
    fscale = 1.0

    def __init__(self, **overrides):
        self.P0 = dict(self.defaults)
        self.P0.update(overrides)

    def times(self, num=float):
        return [num(i) / num(self.nsteps) for i in range(self.nsteps + 1)]

    def u0(self, P):
        return [0 * P[next(iter(P))]] * self.ndof

    def bc(self, t, P):
        raise NotImplementedError

    def q0(self, P):
        raise NotImplementedError

    def oracle(self, t, tp, u_prev, u, q_prev, P):
        raise NotImplementedError

    def tangent(self, t, tp, u_prev, u, q_prev, P):
        raise NotImplementedError


class LinearSprings(Model):
    """(a) dof0 --k1-- dof1 --k2-- dof2 ; dof0 prescribed u0 = t*U0 ; F = t*F0 at dof2."""

    name = "A_linear_2dof_bc"
    ndof, con, free, nsteps = 3, [0], [1, 2], 5
    defaults = {"k1": 100.0, "k2": 50.0, "F0": 10.0, "U0": 0.02}
    fscale = 10.0

    def bc(self, t, P):
        return [t * P["U0"]]

    def q0(self, P):
        return ()

    def oracle(self, t, tp, u_prev, u, q_prev, P):
        s1 = P["k1"] * (u[1] - u[0])
        s2 = P["k2"] * (u[2] - u[1])
        return [-s1, s1 - s2, s2], [0 * t, 0 * t, t * P["F0"]], ()

    def tangent(self, t, tp, u_prev, u, q_prev, P):
        k1, k2 = P["k1"], P["k2"]
        return [[k1, -k1, 0], [-k1, k1 + k2, -k2], [0, -k2, k2]]

    def analytic(self, t, p, k):
        """d^k u_free / dp^k, closed form."""
        P = self.P0
        F = t * P["F0"]
        if p == "k1":
            v = F * (-1) ** k * math.factorial(k) / P["k1"] ** (k + 1)
            return [v, v]
        if p == "k2":
            return [0.0, F * (-1) ** k * math.factorial(k) / P["k2"] ** (k + 1)]
        if p == "F0":
            return [t / P["k1"], t / P["k1"] + t / P["k2"]] if k == 1 else [0.0, 0.0]
        if p == "U0":
            return [t, t] if k == 1 else [0.0, 0.0]
        raise KeyError(p)


class ViscoHardeningChain(Model):
    """(b) 3 springs in series, cubic hardening + smooth viscous internal variable.

    N_e = k_e d + beta d^3 + c (d - q_e),  q_e^{n+1} = (q_e^n + th d)/(1+th),
    th = dt/tau (backward Euler).  Smooth and path dependent.
    dof0 prescribed t*U0, F(t) = F0 sin(pi t) at dof3 (load and unload).
    """

    name = "B_visco_hardening_chain"
    ndof, con, free, nsteps = 4, [0], [1, 2, 3], 20
    defaults = {"k1": 100.0, "beta": 4.0e4, "c": 60.0, "tau": 0.15, "F0": 25.0, "U0": 0.01}
    fscale = 25.0
    kfix = (200.0, 150.0)

    def bc(self, t, P):
        return [t * P["U0"]]

    def q0(self, P):
        z = 0 * P["k1"]
        return (z, z, z)

    def _ks(self, P):
        return (P["k1"], self.kfix[0] + 0 * P["k1"], self.kfix[1] + 0 * P["k1"])

    def oracle(self, t, tp, u_prev, u, q_prev, P):
        th = (t - tp) / P["tau"]
        fint = [0 * t] * 4
        qn = []
        for e, ke in enumerate(self._ks(P)):
            d = u[e + 1] - u[e]
            q = (q_prev[e] + th * d) / (1 + th)
            N = ke * d + P["beta"] * d ** 3 + P["c"] * (d - q)
            fint[e] -= N
            fint[e + 1] += N
            qn.append(q)
        pi = mpmath.pi if isinstance(t, mpmath.mpf) else math.pi
        fext = [0 * t, 0 * t, 0 * t, P["F0"] * _sin(pi * t)]
        return fint, fext, tuple(qn)

    def tangent(self, t, tp, u_prev, u, q_prev, P):
        th = (t - tp) / P["tau"]
        K = [[0 * t] * 4 for _ in range(4)]
        for e, ke in enumerate(self._ks(P)):
            d = u[e + 1] - u[e]
            kt = ke + 3 * P["beta"] * d ** 2 + P["c"] / (1 + th)
            K[e][e] += kt
            K[e + 1][e + 1] += kt
            K[e][e + 1] -= kt
            K[e + 1][e] -= kt
        return K


class ElastoplasticBar(Model):
    """(c) two-bar series, 1-D J2 (linear kinematic hardening) return mapping.

    dof0 fixed, dofs 1,2 free, force F(t) at dof2: 0 -> +F0 (12 incs) -> -F0
    (12 incs).  Element areas A1 = 1, A2 (parameter).  History per element:
    (plastic strain, back stress).  Non-smooth yield activation / reversal.
    """

    name = "C_elastoplastic_bar"
    ndof, con, free, nsteps = 3, [0], [1, 2], 24
    defaults = {"E": 200.0e3, "sigy": 260.0, "H": 2.0e4, "F0": 300.0, "A2": 0.8}
    fscale = 300.0

    def load(self, t, P):
        # t in (0, 1]; peak at t = 0.5
        return P["F0"] * (2 * t) if t <= 0.5 else P["F0"] * (1 - 4 * (t - 0.5))

    def bc(self, t, P):
        return [0 * t]

    def q0(self, P):
        z = 0 * P["E"]
        return ((z, z), (z, z))

    def _rm(self, eps, ep, b, P):
        E, H, sy = P["E"], P["H"], P["sigy"]
        st = E * (eps - ep)
        xi = st - b
        f = abs(xi) - sy
        # yield tolerance (as in production return maps): a state returned to
        # the surface must not re-register as plastic because of round-off,
        # otherwise Newton cycles between elastic/plastic tangents on unloading
        if f > 1e-9 * sy:
            n = 1 if xi > 0 else -1
            dg = f / (E + H)
            return st - E * dg * n, (ep + dg * n, b + H * dg * n), E * H / (E + H)
        return st, (ep, b), E

    def _elems(self, P):
        return ((0, 1, 1 + 0 * P["A2"]), (1, 2, P["A2"]))

    def oracle(self, t, tp, u_prev, u, q_prev, P):
        fint = [0 * t] * 3
        qn = []
        for e, (a, b, A) in enumerate(self._elems(P)):
            sig, q, _ = self._rm(u[b] - u[a], q_prev[e][0], q_prev[e][1], P)
            N = sig * A
            fint[a] -= N
            fint[b] += N
            qn.append(q)
        return fint, [0 * t, 0 * t, self.load(t, P)], tuple(qn)

    def tangent(self, t, tp, u_prev, u, q_prev, P):
        K = [[0 * t] * 3 for _ in range(3)]
        for e, (a, b, A) in enumerate(self._elems(P)):
            _, _, Et = self._rm(u[b] - u[a], q_prev[e][0], q_prev[e][1], P)
            k = Et * A
            K[a][a] += k
            K[b][b] += k
            K[a][b] -= k
            K[b][a] -= k
        return K


MODELS = {"A": LinearSprings, "B": ViscoHardeningChain, "C": ElastoplasticBar}


# --------------------------------------------------------------------------
# oracle adapter with output-precision emulation
# --------------------------------------------------------------------------

def oracle_residual(model, t, tp, u_prev, u, q_prev, P, rf="res64"):
    """Return (r over all dofs, q_new).

    rf = 'res64'  : r delivered in double (.fil / .dat / double ODB)
         'res32'  : r = -RF computed in double, stored float32 (ODB default)
         'fint32' : only the internal-force part is stored float32 and the
                    external load is added back in post (worst case when RF
                    excludes loads applied at held dofs)
    """
    fint, fext, qn = model.oracle(t, tp, u_prev, u, q_prev, P)
    if rf == "res64":
        r = [fe - fi for fe, fi in zip(fext, fint)]
    elif rf == "res32":
        r = [f32(fe - fi) for fe, fi in zip(fext, fint)]
    elif rf == "fint32":
        r = [fe - f32(fi) for fe, fi in zip(fext, fint)]
    else:
        raise ValueError(rf)
    return r, qn


def params_at(P0, d, t):
    return {k: v + t * d.get(k, 0) for k, v in P0.items()}


# --------------------------------------------------------------------------
# nominal Newton solve (the "Abaqus nominal job") and full reruns
# --------------------------------------------------------------------------

def newton_solve(model, P, tol, times=None, maxit=60, stop_residual=None):
    """Incremental Newton; returns dict(us, qs, Ks, rhos, times, iters).

    tol: absolute stopping tolerance on ||r_F||_inf (Abaqus-like R_n * q~).
    stop_residual: if given, emulate a Newton that stopped with a residual of
    that size: after tight convergence the accepted state is offset by
    du = K_FF^{-1} (stop_residual * v), v a fixed unit-ish vector, and the
    history is committed at that (slightly unbalanced) state.
    """
    num = type(P[next(iter(P))])
    ts = times if times is not None else model.times(num)
    u = list(model.u0(P))
    q = model.q0(P)
    us, qs, Ks, rhos, its = [list(u)], [q], [None], [None], [0]
    F = model.free
    for i in range(1, len(ts)):
        t, tp = ts[i], ts[i - 1]
        ucur = list(us[-1])
        for c, v in zip(model.con, model.bc(t, P)):
            ucur[c] = v
        for it in range(maxit):
            fint, fext, _ = model.oracle(t, tp, us[-1], ucur, q, P)
            rF = [fext[j] - fint[j] for j in F]
            if max(abs(x) for x in rF) <= tol:
                break
            K = model.tangent(t, tp, us[-1], ucur, q, P)
            du = solve_dense(submatrix(K, F, F), rF)
            for j, dj in zip(F, du):
                ucur[j] += dj
        else:
            raise RuntimeError(f"Newton failed at increment {i}")
        if stop_residual is not None:
            K = model.tangent(t, tp, us[-1], ucur, q, P)
            v = [stop_residual * (1.0 if (j + i) % 2 == 0 else -0.7) for j in range(len(F))]
            du = solve_dense(submatrix(K, F, F), v)
            for j, dj in zip(F, du):
                ucur[j] -= dj
        fint, fext, qn = model.oracle(t, tp, us[-1], ucur, q, P)
        Ks.append(model.tangent(t, tp, us[-1], ucur, q, P))
        rhos.append([fe - fi for fe, fi in zip(fext, fint)])
        us.append(ucur)
        qs.append(qn)
        q = qn
        its.append(it)
    return {"us": us, "qs": qs, "Ks": Ks, "rhos": rhos, "times": ts, "iters": its}


def stencil_weights(offsets, k, baked_zero=()):
    """Exact weights w with sum_m w_m m^j = delta_jk for the first len(offsets)
    powers j not in baked_zero.  sum_m w_m g(m h) / h^k  ->  [h^k] g."""
    powers = [j for j in range(64) if j not in baked_zero][: len(offsets)]
    if k not in powers:
        raise ValueError("order not resolvable by this stencil")
    n = len(offsets)
    A = [[Fraction(m) ** j for m in offsets] for j in powers]
    rhs = [Fraction(1 if j == k else 0) for j in powers]
    # exact rational solve
    M = [A[r] + [rhs[r]] for r in range(n)]
    for c in range(n):
        p = next(r for r in range(c, n) if M[r][c] != 0)
        M[c], M[p] = M[p], M[c]
        for r in range(n):
            if r != c and M[r][c] != 0:
                f = M[r][c] / M[c][c]
                M[r] = [a - f * b for a, b in zip(M[r], M[c])]
    return [M[r][n] / M[r][r] for r in range(n)]


def full_rerun_derivs(model, P0, d, h, K, tol, times=None, u_read32=False):
    """Full nonlinear reruns at P0 + m h d (central stencils); returns
    derivs[k][i] = d^k u_i / dt^k (all dofs) for k = 1..K."""
    offs = {1: [-1, 1], 2: [-1, 0, 1], 3: [-2, -1, 1, 2], 4: [-2, -1, 0, 1, 2]}
    need = sorted({m for k in range(1, K + 1) for m in offs[k]})
    sols = {}
    for m in need:
        s = newton_solve(model, params_at(P0, d, m * h), tol, times=times)
        us = s["us"]
        if u_read32:
            us = [[f32(x) for x in u] for u in us]
        sols[m] = us
    n = len(sols[need[0]])
    out = {}
    for k in range(1, K + 1):
        w = stencil_weights(offs[k], k)
        fk = math.factorial(k)
        rows = []
        for i in range(n):
            acc = [0 * h] * model.ndof
            for m, wm in zip(offs[k], w):
                wm_num = type(h)(wm.numerator) / type(h)(wm.denominator)
                acc = [a + wm_num * x for a, x in zip(acc, sols[m][i])]
            rows.append([fk * a / h ** k for a in acc])
        out[k] = rows
    return out


def hp_reference(model, P0, d, K, dps=80, h="1e-12"):
    """High-precision (mpmath) full-rerun reference derivatives along d."""
    with mpmath.workdps(dps):
        Pm = {k: mpf(repr(v)) for k, v in P0.items()}
        dm = {k: mpf(repr(v)) for k, v in d.items()}
        tol = mpf(10) ** (-(dps - 20)) * mpf(model.fscale)
        der = full_rerun_derivs(model, Pm, dm, mpf(h), K, tol)
        return {k: [[float(x) for x in row] for row in rows] for k, rows in der.items()}


def hp_reference_mixed(model, P0, pa, pb, ka, kb, dps=80, h="1e-12"):
    """High-precision tensor-product central reference for
    d^(ka+kb) u / (d pa^ka d pb^kb) (physical units)."""
    offs = {1: [-1, 1], 2: [-1, 0, 1], 3: [-2, -1, 1, 2]}
    with mpmath.workdps(dps):
        Pm = {k: mpf(repr(v)) for k, v in P0.items()}
        ha = mpf(h) * abs(Pm[pa])
        hb = mpf(h) * abs(Pm[pb])
        tol = mpf(10) ** (-(dps - 20)) * mpf(model.fscale)
        wa = stencil_weights(offs[ka], ka)
        wb = stencil_weights(offs[kb], kb)
        acc = None
        for ma, wma in zip(offs[ka], wa):
            for mb, wmb in zip(offs[kb], wb):
                P = dict(Pm)
                P[pa] += ma * ha
                P[pb] += mb * hb
                us = newton_solve(model, P, tol)["us"]
                w = mpf(wma.numerator) / wma.denominator * mpf(wmb.numerator) / wmb.denominator
                term = [[w * x for x in u] for u in us]
                acc = term if acc is None else [[a + b for a, b in zip(r1, r2)] for r1, r2 in zip(acc, term)]
        scale = math.factorial(ka) * math.factorial(kb) / (ha ** ka * hb ** kb)
        return [[float(scale * x) for x in row] for row in acc]


# --------------------------------------------------------------------------
# the probe/commit recursion (the algorithm under review)
# --------------------------------------------------------------------------

SCHEMES = {
    # order k -> (offsets, baked-zero powers).  0 is the nominal replay probe.
    "central": {1: ([-1, 1], ()), 2: ([-1, 0, 1], ()), 3: ([-2, -1, 1, 2], ()), 4: ([-2, -1, 0, 1, 2], ())},
    "central4": {1: ([-2, -1, 1, 2], ()), 2: ([-2, -1, 0, 1, 2], ()), 3: ([-2, -1, 1, 2], ()), 4: ([-2, -1, 0, 1, 2], ())},
    "forward": {1: ([0, 1], ()), 2: ([0, 1, 2], ()), 3: ([0, 1, 2, 3], ())},
    # "baked" stencils rely on the predictor having nulled lower Taylor terms
    "forward_baked": {1: ([0, 1], ()), 2: ([0, 1], (1,)), 3: ([0, 1], (1, 2))},
    "central_baked": {1: ([-1, 1], ()), 2: ([-1, 0, 1], ()), 3: ([-1, 1], (1, 2))},  # power 2 cancels by symmetry
}


def read_nominal(model, nominal, u_read32=False):
    us = nominal["us"]
    if u_read32:
        us = [[f32(x) for x in u] for u in us]
    return us


def recursion(model, P0, d, h, K, scheme="central", *, nominal, u_read32=False, rf="res64",
              g0_mode="replay", history_mode="branch", bc_mode="exact", K_scale=None,
              defect_iters=0, seed="none", order_iters=0, iter_tol=1e-8,
              adaptive_thr=None):
    """Probe/commit recursion along direction d.

    seed="prev": the order-k probes are shifted by t^k/k! * coef_{i-1}^(k)
    (previous-increment coefficient on the free dofs) and the solve returns the
    correction: coef_i^(k) = seed + k! K^-1 c_k.  Exact identity; keeps the
    probe state close to the perturbed path (fewer spurious active-set flips).
    order_iters > 0: re-probe each order at the updated estimate until the
    relative correction is <= iter_tol (fixed-point / chord iteration on the
    order-k Taylor equation; also corrects an inexact K).  Returns dict with
    coef[k][i] (all dofs, d^k u/dt^k), pred_res[i] (max_m ||r_F(commit)||_inf),
    asym[i] (order-1 forward/backward one-sided disagreement), runs (oracle
    calls excluding the shared nominal replay)."""
    ts = nominal["times"]
    N = len(ts) - 1
    F, C = model.free, model.con
    us_read = read_nominal(model, nominal, u_read32)
    sch = SCHEMES[scheme]
    branch_offsets = sorted({m for k in range(1, K + 1) for m in sch[k][0] if m != 0})
    weights = {k: [(m, w) for m, w in zip(sch[k][0], stencil_weights(sch[k][0], k, sch[k][1])) if w != 0]
               for k in range(1, K + 1)}
    if K_scale is not None and len(F) > 0:
        rng = np.random.default_rng(1234)
        Kpert = 1 + K_scale * rng.standard_normal((len(F), len(F)))
        Kpert = (Kpert + Kpert.T) / 2
    br = {m: {"u": list(model.u0(params_at(P0, d, m * h))), "q": model.q0(P0), "P": params_at(P0, d, m * h)}
          for m in branch_offsets}
    zero = {"u": list(model.u0(P0)), "q": model.q0(P0)}
    coef = {k: [[0.0] * model.ndof] for k in range(0, K + 1)}
    coef[0] = [list(us_read[0])]
    pred_res, asym, runs = [0.0], [0.0], 0
    asymc, pnorm = [0.0], [0.0]
    smax_run = {k: 0.0 for k in range(1, K + 1)}
    iters_used = {k: [0] for k in range(1, K + 1)}
    last_delta = {k: [0.0] for k in range(1, K + 1)}
    # affine BC Taylor coefficients along t
    for i in range(1, N + 1):
        t, tp = ts[i], ts[i - 1]
        ustar = list(us_read[i])
        # prescribed dofs always come from the (exact) input deck, never from
        # the (possibly float32) result file -- the same values the probes use
        for c, v in zip(C, model.bc(t, P0)):
            ustar[c] = v
        rz, qz = oracle_residual(model, t, tp, zero["u"], ustar, zero["q"], P0, rf)
        if g0_mode == "replay":
            g0 = [rz[j] for j in F]
        elif g0_mode == "newton":
            g0 = [nominal["rhos"][i][j] for j in F]
        elif g0_mode == "zero":
            g0 = [0.0] * len(F)
        else:
            raise ValueError(g0_mode)
        zprev = zero["u"]
        zero["u"], zero["q"] = ustar, qz
        Kfull = nominal["Ks"][i]
        KFF = submatrix(Kfull, F, F)
        if K_scale is not None:
            KFF = [[KFF[a][b] * Kpert[a][b] for b in range(len(F))] for a in range(len(F))]
        bc0 = model.bc(t, P0)
        bc1 = model.bc(t, params_at(P0, d, 1.0))
        bcd = [b1 - b0 for b0, b1 in zip(bc0, bc1)]  # affine BC: exact first coefficient
        for k in range(0, K + 1):
            coef[k].append([0.0] * model.ndof)
        coef[0][i] = ustar
        gsave = {}
        for k in range(1, K + 1):
            # current estimate of the free-dof k-th coefficient (probe shift)
            est = [coef[k][i - 1][j] if seed == "prev" else 0.0 for j in F]
            for it in range(order_iters + 1):
                acc = [0.0] * len(F)
                for m, w in weights[k]:
                    wf = float(w)
                    if m == 0:
                        g = g0
                    else:
                        tm = m * h
                        Pm = params_at(P0, d, tm)
                        ucur = [sum(tm ** j / math.factorial(j) * coef[j][i][x] for j in range(k)) for x in range(model.ndof)]
                        for x, ev in zip(F, est):
                            ucur[x] += tm ** k / math.factorial(k) * ev
                        if bc_mode == "exact":
                            for c, v in zip(C, model.bc(t, Pm)):
                                ucur[c] = v
                        else:  # 'kfc': constrained dofs held at nominal in the probe (spec item 4)
                            for c in C:
                                ucur[c] = ustar[c]
                        if history_mode == "branch":
                            hist = br[m]
                        elif history_mode == "frozen":
                            hist = {"u": zprev, "q": nominal["qs"][i - 1]}
                        elif history_mode == "shared":
                            hist = br[max(branch_offsets)]
                        else:
                            raise ValueError(history_mode)
                        r, _ = oracle_residual(model, t, tp, hist["u"], ucur, hist["q"], Pm, rf)
                        runs += 1
                        g = [r[j] for j in F]
                        if k == 1:
                            gsave[m] = g
                    acc = [a + wf * x for a, x in zip(acc, g)]
                ck = [a / h ** k for a in acc]
                rhs = [math.factorial(k) * x for x in ck]
                if k == 1 and bc_mode == "kfc":
                    KFC = submatrix(Kfull, F, C)
                    rhs = [rr - sum(KFC[a][b] * bcd[b] for b in range(len(C))) for a, rr in enumerate(rhs)]
                delta = solve_dense(KFF, rhs)
                est = [a + b for a, b in zip(est, delta)]
                scale_k = max(max(abs(x) for x in est), smax_run[k], 1e-300)
                dl = max(abs(x) for x in delta) / scale_k
                if dl <= iter_tol:
                    break
                if adaptive_thr is not None:
                    # adaptive mode: accept the first probe set unless the order-1
                    # forward/backward asymmetry signals an active-set flip
                    if k > 1:
                        break
                    if 1 in gsave and -1 in gsave:
                        fw_ = solve_dense(KFF, [(a - b) / h for a, b in zip(gsave[1], g0)])
                        bw_ = solve_dense(KFF, [(b - a) / h for a, b in zip(gsave[-1], g0)])
                        A_ = max(abs(a - b) for a, b in zip(fw_, bw_)) / scale_k
                        if it == 0 and A_ <= adaptive_thr:
                            break
            iters_used[k].append(it)
            last_delta[k].append(dl)
            smax_run[k] = max(smax_run[k], max(abs(x) for x in est))
            row = [0.0] * model.ndof
            for j, v in zip(F, est):
                row[j] = v
            for c, v in zip(C, bcd if k == 1 else [0.0] * len(C)):
                row[c] = v
            coef[k][i] = row
        # order-1 one-sided asymmetry indicator (free: uses existing probes)
        if 1 in gsave and -1 in gsave:
            fw = solve_dense(KFF, [(a - b) / h for a, b in zip(gsave[1], g0)])
            bw = solve_dense(KFF, [(b - a) / h for a, b in zip(gsave[-1], g0)])
            asym.append(max(abs(a - b) for a, b in zip(fw, bw)))  # normalized below
        else:
            asym.append(float("nan"))
        # optional defect correction (order 1, central branches): re-commit
        for _ in range(defect_iters if (K == 1 and set(branch_offsets) == {-1, 1}) else 0):
            cres = {}
            for m in (-1, 1):
                tm = m * h
                Pm = params_at(P0, d, tm)
                ucom = [coef[0][i][x] + tm * coef[1][i][x] for x in range(model.ndof)]
                for c, v in zip(C, model.bc(t, Pm)):
                    ucom[c] = v
                r, _ = oracle_residual(model, t, tp, br[m]["u"], ucom, br[m]["q"], Pm, rf)
                runs += 1
                cres[m] = [r[j] for j in F]
            corr = solve_dense(KFF, [(a - b) / (2 * h) for a, b in zip(cres[1], cres[-1])])
            for j, v in zip(F, corr):
                coef[1][i][j] += v
        # commits
        pr = 0.0
        cfin = {}
        for m in branch_offsets:
            tm = m * h
            Pm = br[m]["P"]
            ucom = [sum(tm ** j / math.factorial(j) * coef[j][i][x] for j in range(K + 1)) for x in range(model.ndof)]
            for c, v in zip(C, model.bc(t, Pm)):
                ucom[c] = v
            r, qn = oracle_residual(model, t, tp, br[m]["u"], ucom, br[m]["q"], Pm, rf)
            runs += 1
            pr = max(pr, max(abs(r[j]) for j in F))
            cfin[m] = [r[j] for j in F]
            br[m]["u"], br[m]["q"] = ucom, qn
        pred_res.append(pr)
        if 1 in cfin and -1 in cfin:
            fwc = solve_dense(KFF, [(a - b) / h for a, b in zip(cfin[1], g0)])
            bwc = solve_dense(KFF, [(b - a) / h for a, b in zip(cfin[-1], g0)])
            asymc.append(max(abs(a - b) for a, b in zip(fwc, bwc)))
            pnorm.append(max(abs(sum(KFF[a][b] * coef[1][i][F[b]] for b in range(len(F)))) for a in range(len(F))))
        else:
            asymc.append(float("nan"))
            pnorm.append(float("nan"))
    # normalize the asymmetry by the history-wide max |s| (per-increment |s| can vanish)
    smax = max(max(abs(coef[1][i][j]) for j in F) for i in range(1, N + 1)) or 1e-300
    asym = [a / smax for a in asym]
    asymc = [a / smax for a in asymc]
    # relative predictor defect: ||r_F(commit)|| / (h * max_i ||K_FF s_i||)
    pmax = max([x for x in pnorm[1:] if x == x] or [1.0]) or 1e-300
    pred_rel = [pr_ / (h * pmax) for pr_ in pred_res]
    return {"coef": coef, "pred_res": pred_res, "asym": asym, "asym_commit": asymc, "pred_rel": pred_rel,
            "runs": runs, "branches": branch_offsets, "iters": iters_used, "last_delta": last_delta}


# --------------------------------------------------------------------------
# error metrics
# --------------------------------------------------------------------------

def history_rel_error(model, est, ref, free_only=True, start=1, uscale=None):
    """max_i max_j |est - ref| / max_i max_j |ref| over all increments.

    If the reference is (numerically) identically zero -- e.g. d2u/dF0^2 of a
    problem linear in F0 -- the absolute error is divided by uscale (the
    nominal displacement scale; coefficients along t = relative parameter
    change carry displacement units) instead."""
    J = model.free if free_only else range(model.ndof)
    num = max(abs(est[i][j] - ref[i][j]) for i in range(start, len(ref)) for j in J)
    den = max(abs(ref[i][j]) for i in range(start, len(ref)) for j in J)
    if uscale is not None and den < 1e-12 * uscale:
        return num / uscale
    return num / den if den > 0 else num


def u_scale(model, nominal):
    return max(abs(x) for u in nominal["us"] for x in u)


def scale_coef(rows, s):
    return [[x / s for x in row] for row in rows]
