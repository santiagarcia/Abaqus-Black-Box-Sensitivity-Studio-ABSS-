# Gauss review: residual-oracle, history-consistent sensitivities (spec section 3)

Reviewer: Gauss (numerical methods / sensitivities), independent reviewer #2.
Date: 2026-10-16. Council snapshot `1d52a2105ab2...` (project `abss`). Affect layer: enabled, low influence (curiosity drove the extra seeding/iteration experiments in sections 6.3 and 6.4; no effect on evidence thresholds).
Independence: I did not read `docs/council/*` by other members, Noether's `validation/reference_models/noether/*`, or Curie's/Scout's experiment results. I read the spec (sections 0, 2, 3, 6, 7) and the user's `pyresfd/core.py` as prior art (no code copied). I read one line-range of Scout's raw `abaqus help` dump only to confirm that an `output_precision` execution option exists.

Status of everything below: **verified on manufactured Python oracles only**. Nothing here has been run against Abaqus. Every number is reproducible from `validation/reference_models/gauss/` (commands in section 11).

---

## 1. What is differentiated

* Increment map: accepted increments i = 1..N on the **nominal accepted time grid**, with all-DOF state u_i, history q_i, parameter a. `r_i(u_i; u_{i-1}, q_{i-1}, a) = f_ext - f_int`, `K_i = -dr_i/du_i` (positive stiffness). At a prescribed DOF, r = -RF.
* Target: total derivatives `u_i^(k) = d^k u_i / da^k` (and directional/mixed variants) of the **discrete converged incremental solution**, for k = 1..3, at every increment. Branch of the solution fixed (same active set, same time grid).
* Directional form used throughout: P(t) = P0 + t d, coefficients `c_i^(k) = d^k u_i/dt^k`. For a pure parameter with d = |a| e_a, t is the relative step `h_rel`.

## 2. Derivation (what must hold for the recursion to be exact)

Let `U_j(t)`, j < i, be the displacements a branch was committed at, and `q_{i-1}(t)` the history the oracle builds from them. Define the order-k probe function

    g_k(t) = r_i( u_i^[<k](t) ,  history of branch at P0+t d ,  P0 + t d ),     u_i^[<k](t) = sum_{j<k} t^j/j! c_i^(j)

with constrained DOFs set to their exact perturbed prescribed values. If each branch's past displacements equal the true perturbed solution to O(t^{k+1}) (i.e. **the branch was committed with all orders <= K >= k at every earlier increment**), then because r_i(u_i(t), exact history, P(t)) == 0,

    g_k(t) = K_FF * t^k/k! * c_i^(k)  +  O(t^{k+1})      =>   K_FF c_i^(k) = k! [t^k] g_k .

This is the spec's recursion (sign convention confirmed). Three consequences the spec states only implicitly:

1. **Every stencil offset m*h is its own complete history branch from t = 0.** The history inside g_k(m h) must be the history of the solution at P0 + m h d. A branch at +h cannot serve probes at -h (S6: error 0.2-1.0 at order 1, up to 5e4 at order 2), and probing with the nominal history (no propagation) is wrong for every history-carrying parameter (S6: 0.22-1.0).
2. All branches of one direction must advance **in lockstep**: commits at increment i need `c_i^(1..K)`, which need probes on all branches at increment i.
3. `g_k(0)` is not zero. It is the residual of the *read* nominal state (Newton tolerance, float32 rounding of u*). It must be **measured** by a probe on a nominal **replay branch** (all DOFs prescribed to the nominal u* as read, with prescribed DOFs from the input deck), not assumed 0 and not taken from the Newton log.

Constrained DOFs: putting the exact perturbed BC value in the probe (`bc_mode=exact`) and the spec's form (probe at nominal u_C, RHS `p_F - K_FC s_C`) are both verified at order 1 (S13: both <= 7e-10 at h = 1e-4..1e-6; the K_FC form has no truncation for a BC parameter, 2e-14 vs 2.5e-6 at h = 1e-2). I recommend the exact-BC form as default because it needs no K_FC (which may be unavailable when Abaqus eliminates prescribed DOFs) and extends unchanged to k >= 2.

## 3. Prototype

`validation/reference_models/gauss/abss_gauss_ref.py` (library) and `run_studies.py` (16 studies, outputs in `outputs/S*.txt`).
The algorithm only calls `oracle(t_i, t_{i-1}, u_prev, u_cur, q_prev, P) -> (f_int, f_ext, q_new)` (one frozen-displacement increment, every DOF prescribed: the analogue of a one-increment Abaqus restart with RF output) plus the nominal tangent K_i.

Manufactured models:

| id | model | DOFs | incs | history | parameters |
|---|---|---|---|---|---|
| A | linear springs, prescribed u0 = t U0 (parameter-dependent BC), F = t F0 | 1 C + 2 F | 5 | none | k1, k2, F0, U0 |
| B | 3-spring chain, cubic hardening `k d + beta d^3`, smooth viscous internal variable (backward Euler, tau), load-unload F0 sin(pi t), prescribed u0 = t U0 | 1 C + 3 F | 20 | smooth | k1, beta, c, tau, F0, U0 |
| C | 2-bar series, 1-D J2 return map, linear kinematic hardening, force control 0 -> +300 -> -300 (yield, unload, reverse yield) | 1 C + 2 F | 24 | plastic strain, back stress (non-smooth) | E, sigy, H, F0, A2 |

References: A closed form; B, C **80-digit mpmath full nonlinear reruns** (Newton to 1e-60, central FD with h = 1e-12) — independent of the recursion code path (shares only the model definition). The model's consistent tangent was checked against frozen-history central FD of the oracle: max rel. difference 5e-10 / 1e-9 / 1e-8 (S0); cond2(K_FF) <= 19.

## 4. Verification results (double precision, central stencils, default `g0 = replay`)

Error metric: max over **all increments** and free DOFs of |est - ref|, divided by max |ref| over the history (absolute / max|u| when the reference is identically 0).

### 4.1 Best error over the h sweep (h_rel = 1e-1 ... 1e-10), recursion vs full-rerun central FD

| model/param | order 1 rec (h) | order 1 full-FD (h) | order 2 rec (h) | order 2 full-FD (h) | order 3 rec (h) |
|---|---|---|---|---|---|
| A / k1 | 3e-16 (1e-1) | 1e-10 (1e-5) | 4e-15 (1e-1) | 2.8e-8 (1e-4) | 2e-14 (1e-1) |
| B / k1 | 7e-14 (1e-2) | 9e-11 (1e-5) | 1.5e-13 (1e-1) | 8.5e-8 (1e-3) | 4.4e-6 (1e-3) |
| B / tau | 4.1e-11 (1e-5) | 1.4e-10 (1e-5) | 1.2e-7 (1e-4) | 1.8e-7 (1e-3) | 1.6e-6 (1e-3) |
| B / beta | 8e-14 (1e-2) | 6e-11 (1e-5) | 3.3e-8 (1e-3) | 6.8e-8 (1e-4) | 1.2e-6 (1e-3) |
| C / E | 1.7e-10 (1e-5) | 1.2e-10 (1e-5) | 1.6e-7 (1e-4) | 1.1e-7 (1e-4) | 4.3e-6 (1e-3) |
| C / H | 5.7e-11 (1e-5) | 1e-10 (1e-5) | 2.3e-8 (1e-3) | 5e-8 (1e-4) | 1.4e-7 (1e-3) |
| C / sigy | 1.1e-13 (1e-3) | 1.3e-14 (1e-2) | 5.6e-12 (1e-2) | 7.0e-12 (1e-2) | 7.5e-7 (1e-3) |
| C / A2 | 1.5e-13 (1e-3) | 8e-11 (1e-6) | 3.7e-8 (1e-4) | 3.2e-8 (1e-4) | 9.3e-7 (1e-3) |

(S1, S2, S3.) The recursion is never worse than full-rerun FD in its plateau and is often far better: its truncation is governed by the nonlinearity of r in (a, history) at fixed u, not by the nonlinearity of u(a). Error curves show the classic V shape; the observed optima match `h_opt ~ eps^(1/(k+2))` with eps ~ 1e-16: 5e-6, 1e-4, 6e-4 for k = 1, 2, 3, with errors ~1e-11, ~1e-8, ~1e-6.

### 4.2 Plateaus (smooth regime, double)

* Order 1: error <= 1e-8 for h_rel in [1e-6, 1e-4] for every parameter of B and C; <= 1e-6 over [1e-7, 1e-3].
* Order 2: <= 1e-6 over [1e-4, 1e-3] for every parameter of B and C (at 1e-2: up to 1.3e-5 in B, 9e-5 in C).
* Order 3: <= 5e-6 only at h_rel ~ 1e-3; at 1e-2 up to 1e-4 (truncation), at 1e-4 3.6e-5..2.2e-3 (round-off), at 1e-5 6e-2..4. Narrow window.
* Model C at h_rel = 1e-2 for sigy/F0/A2: order 1 errors 0.1-0.23 (probe active-set flip, section 6.3), even though the true solution keeps its active set (full-FD 1e-14).

### 4.3 Predictor (commit) residual scaling

`max_i ||r_F(u_i^[<=K](h))|| / fscale` log-slopes per decade (S1-S3, S14):
* K = 1: 2.00 (A/k1, A/k2, all B parameters except U0, C/E, C/H, C/A2) until the floor (C/sigy, C/F0 are piecewise linear: residual already at round-off) (= nominal Newton residual 1e-12 * fscale in B; ~1e-15 in A, C).
* K = 2: 3.00.
* K = 3 with the standard central stencils: **3.0-3.3, not 4** (B, C); with the O(h^4) order-1/2 stencils on the same 4 branches (`central4`): 4.00-4.05. The commit residual is `O(h^{min_k(k + p_k)})`, p_k = truncation order of the order-k stencil, so for central stencils the gate must expect slope min(K+1, 3). Model A (r affine in each parameter) shows 2/3/4.

### 4.4 Mixed derivatives (S11) — see section 7. Best errors: d2u/dadb 2e-8 to 3e-7 (h 1e-3..1e-4); d3u/da2db and d3u/dadb2 1.8e-7 to 3.8e-6 (h 1e-3).

## 5. Stencils (high-order coefficient extraction)

Weights solve `sum_m w_m m^j = delta_jk` exactly (rationals) for the first n powers; `[t^k] g ~ sum_m w_m g(m h) / h^k`; `c^(k) = k! K^-1 [t^k]g`. Offset 0 is the replay probe (shared by all directions; for odd k its weight is 0).

| order k | recommended offsets | weights (m: w) | truncation | branches needed (nonzero offsets) |
|---|---|---|---|---|
| 1 | ±1 | -1: -1/2, +1: +1/2 | O(h^2) | 2 |
| 1 (when ±2 exist) | ±1, ±2 | -2: 1/12, -1: -2/3, 1: 2/3, 2: -1/12 | O(h^4) | 4 |
| 2 | -1, 0, +1 | 1/2, -1, 1/2 (gives [t^2]; c = 2!*that) | O(h^2) | 2 |
| 2 (when ±2 exist) | -2..2 | -1/24, 2/3, -5/4, 2/3, -1/24 | O(h^4) | 4 |
| 3 | ±1, ±2 | -2: -1/12, -1: 1/6, 1: -1/6, 2: 1/12 | O(h^2) | 4 |
| 4 | -2..2 | 1/24, -1/6, 1/4, -1/6, 1/24 | O(h^2) | 4 |

S5 evidence (best error / runs per increment per direction):
* central: k=1 7e-14..1.7e-10 (4 runs), k=2 1.5e-13..1.6e-7 (6), k=3 1.4e-7..4.4e-6 (12).
* central4 (Richardson on ±1, ±2): k=1 6.5e-14..3.5e-12 (h = 1e-2..1e-3, 8 runs), k=2 1.8e-13..2.2e-9 (12 runs). Useful when outputs are noisy (allows larger h) or when ±2 branches exist anyway (K >= 3).
* forward (spec's one-sided, structural): k=1 O(h): 2e-6..9e-6 at h = 1e-5 (except parameters entering r linearly at fixed u, e.g. B/k1 5e-11), best 1.7e-8..8e-8 at h = 1e-7 to 1e-8 (2 runs). Acceptable only as a "preview" mode with double outputs.
* **"Baked" stencils that rely on the predictor having zeroed lower Taylor terms are inadmissible**: one-sided order 2 from `(g(h) - g(0))/h^2` stays at 4e-2 to 1.0 error for all h; reduced 2-point order 3 stays at 0.05-1.0 (S5). Reason: lower-order truncation errors e_j = O(h^p) re-enter g at power t^j and are divided by h^k. Only stencils that annihilate all powers < k by their own weights converge. (Agrees with the comments in pyresfd's core.py; here confirmed in the history-dependent setting.)
* Complex-step / complex-polynomial extraction: not applicable — Abaqus is a real-valued oracle.

**History along each offset**: yes. Each offset m*h in the union of the stencils of orders 1..K is one complete branch from the beginning of the analysis, committed at every increment at `u^[<=K](m h)` with the perturbed parameter and BCs defined at the start of that branch. Orders share branches: K = 1, 2 use {±1}; K = 3, 4 use {±1, ±2}. A single branch at +h with all lower-order predictors is sufficient only for the one-sided order-1 (forward) scheme; every higher or central scheme needs the multiple branches.

## 6. Numerical risks, ranked (with measured impact)

### 6.1 [Critical] Nominal residual and the g(0) term
Abaqus' default force criterion is R_n = 5e-3 of the time-averaged force (recalled default; verify for the installed version). The spec's one-sided `p = [r(u*, a+h) - r(u*, a)]/h` with `r(u*, a)` taken as 0 gives error ~ rho/h:
* S8, B/k1, rho = 1e-8 (much tighter than Abaqus default): one-sided order 1 with g0 = 0 -> 2.5e-1 at h = 1e-6; with replay g0 -> 4.4e-8. Order 2 with g0 = 0: 6e1 at h = 1e-4.
* Even with replay g0 there is an irreducible floor ~ C * rho (the derivative of an off-equilibrium state): rho = 5e-3 -> 2.3e-2 (B), 3.4e-3 (C/E); rho = 1e-5 -> 4.4e-5 / 6.9e-6.
* Recommendation: always measure g0 by the replay probe; run the nominal with a tightened residual control (target ||r_F|| <= 1e-8 of the force scale) and report the measured g0 norm as the accuracy floor.

### 6.2 [Critical for order >= 2] Output precision (ODB float32)
S7 (double = `res64`):
* RF stored float32 **when the stored quantity is the small residual itself** (`res32`): order 1 floor 2e-8..7e-8, order 2 unaffected (6e-8 at h = 1e-3). Cheap.
* RF stored float32 **when the residual must be formed by subtracting large numbers** (`fint32`, e.g. RF excludes a load applied at a held DOF): order 1 error ~6e-8*|f_int|/(h|p|), parameter dependent: 7.8e-4 (B/beta) and 4.1e-3 (C/E) at h = 1e-4, while B/k1 and C/F0 (large |p|) stayed <= 1e-5. Order 2 for B/beta and C/E >= 1.7e-3 at every h (0.15-0.2 at h = 1e-3). Theory: h_opt = (6e-8)^(1/(k+2)) = 4e-3 / 1.6e-2 / 3.6e-2, best errors ~1e-5 / 2e-4 / 1e-3.
* Nominal u* read from float32 (`u32`) is harmless **only if** g0 comes from a replay at the same rounded u* (order 1 6e-8..2e-7, order 2 7e-9..9e-7 at h = 1e-3). With g0 from the Newton log (inconsistent base point) order 2 fails: 1.2 at h = 1e-3 (B/k1). A real bug I hit: prescribed DOFs in the replay must use the exact input-deck values, never file-read values, or order 2 fails (B/k1: 8.2e-2 at h = 1e-3 in the pre-fix run; reproducible by deleting the 3-line BC override in `recursion`) — fixed in the prototype.
* **Full-rerun FD references with float32 U are themselves unusable**: order 1 4.3e-3 at h = 1e-4 (B/k1); order 2 best 8e-4 (h = 1e-1..1e-2) and 0.08-1.4 at h = 1e-3. Validation references need double U.
* Recommendation: request double precision for all quantities entering a cancellation (RF, U, CF, and fields used for output sensitivities). The installed Abaqus lists an execution option `output_precision={single | full}` (Scout's `abaqus help` dump, line 36); which output it covers (nodal only?) is **unverified** — Scout/Curie must confirm. The `.fil` (`*NODE FILE`) route is believed to be double precision and `*NODE PRINT` (.dat) is formatted text with limited digits (possibly worse than float32) — **both unverified**. Until verified, order >= 2 must be disabled when only float32 data are available.

### 6.3 [High] Off-path probes flip the active set (new finding; dissent with spec item 3)
The order-1 probe holds the *current* displacement at the nominal u* while history and a are perturbed. For path-dependent materials this state is not near the perturbed solution: in model C a 1% sigy change shifts the probe trial stress by E*dεp ~ 26 MPa, while the true solution's stress is unchanged. The probe crosses a yield surface the true path does not:
* C (nominal) at h = 1e-2: order 1 error 0.10 (sigy), 0.13 (F0), 0.23 (A2) while full-rerun FD is 1e-14 / 4e-15 / 1e-4 (S3).
* C with a yield-onset 1e-5 (relative) from the nominal: **E direction** (true derivative exists, no active-set change in u(E) under force control) — spec probe error 0.15-0.17 for every h >= 1e-4; full FD 1e-8 (S4).
* Fix (verified): re-probe at the predicted displacement `u* + t s` (fixed-point/chord iteration on the order-k equation, identical solution in the smooth case): E-direction error 8e-9 at h = 1e-4 (S15). Seeding the first probe with the previous increment's coefficient (`seed=prev`) is the same identity; it helps at large h (C/A2 h = 1e-2: 0.24 -> 9e-5) but costs accuracy at large h for smooth parameters (B/k1 h = 0.1: 1e-13 -> 5e-4), so it is not the default (S12).
* Recommended **adaptive** policy (S15): spec probe first; only if the order-1 one-sided asymmetry exceeds max(1e-2, 30 h_rel) re-probe at `u* + t s` until the relative correction <= 1e-8 (max 8). Measured extra cost: 0 in smooth runs for h <= 1e-3 (0.08 for C/A2 at h = 1e-2); 0.5-1.3 extra order-1 probe sets per increment in the kink cases.

### 6.4 [High] Genuine non-smoothness (yield activation/reversal near a, contact, cutback-dependent paths)
S4, sigy direction with all four yield/reversal events within 1e-5 of nominal: for h >= 1e-4 both recursion (0.27) and full FD (0.45-0.5) are wrong at increments 8, 10, 20, 22 (at h = 1e-5 the recursion still is, 0.23, full FD is not); for h <= 1e-6 both are correct (2e-10). Order 2 cannot be resolved there (needs h < 1e-5, round-off gives 2e-3 at h = 1e-6). Exactly at the kink no derivative exists; both methods return an arbitrary secant (0.27 / 0.5) for all h.
Detection (zero extra Abaqus runs): (a) order-1 forward/backward asymmetry `A_i = ||K^-1[(g(h)-g0) - (g0-g(-h))]/h|| / max_i||s||` — smooth: ~1.8 h (C), ~1.65 h (B/tau); kink: 0.15-0.55 independent of h; (b) non-convergence of the 6.3 iteration; (c) relative predictor defect `||r_F(commit)|| / (h max||K s||)` — smooth ~h^2 (K = 2), kink 0.13-0.27. (The prototype does not subtract g0 in (c); at tiny h the nominal residual then dominates, e.g. 5e-6 at h = 1e-6 in B. The C++ gate should subtract it.) All erroneous S15 runs (e1 > 1e-3) were flagged at the exact increments; no smooth run was flagged at h in [1e-6, 1e-3].

### 6.5 [High] Time grid / cutbacks
Branches and probes must use the nominal accepted grid (fixed increments, no automatic incrementation). S9: a full-rerun FD reference whose +h run cut back once in increment 5 gives order-1 error 1.2e1 at h = 1e-4 (rate-dependent B; error ~ discretization difference / h); rate-independent C was unaffected in that elastic increment. The recursion is immune by construction (grid prescribed). Full-rerun validation jobs must also be forced onto the nominal grid.

### 6.6 [Medium] Inexact tangent operator
s inherits K's error amplified by conditioning: a 2% elementwise perturbation of K_FF gave 0.33-0.47 (B, cond ~19) and 0.065-0.079 (C) order-1 error (S10). The same re-probe iteration as 6.3 (or re-commit defect correction) converges linearly with factor ||I - K~^-1 K||: 2% -> 1.3e-2..3e-5 after 3 iterations; 20% diverges. Therefore the spec's decisive gate "operator equals frozen-history directional derivative" must be quantitative: require ||K_op v - K_fd v|| / ||K_fd v|| <= 1e-6 on random v, else enable iteration and report its contraction. Note Abaqus' operator may be the tangent of the last iteration, not at the converged state.

### 6.7 [Medium] Conditioning
Error in s ~ cond(K_FF) * (relative noise in p). cond2 <= 19 here; real meshes 1e5-1e8, so the order-1 floor in double moves from ~1e-11 to ~1e-6..1e-3 relative in the worst-conditioned components; order 2/3 proportionally worse. Report cond estimates (from the factorization) per increment; scale DOFs (rotations vs translations) before solving.

### 6.8 [Low] Step size versus parameter bounds and active-set radius
a ± 2h must stay inside physical bounds (nu < 0.5, positive moduli); for a ~ 0 use a declared scale. h must be smaller than the distance to the nearest active-set change; the gates in 6.4 detect when it is not.

## 7. Mixed derivatives d²u/(da db)

Recommendation: never build cross-coupled branch grids. Run independent **directional** recursions (pure order-k in direction d) and recover mixed terms by polarization:
* `(a+b)` and `(a-b)` directions (d = (|a|, ±|b|)): `u_ab = (D^2_{+} - D^2_{-}) / (4 |a||b|)` — no pure terms needed, 2 directions = 4 branches per pair. Best 2.1e-8..2.7e-7 (S11).
* Or with the pure directions already computed: `u_ab = (D^2_{a+b} - |a|^2 u_aa - |b|^2 u_bb) / (2|a||b|)` — 1 extra direction = 2 branches per pair; more cancellation (best 6.4e-8..9.4e-7).
* Order 3 with directions a, b, a+b, a-b: `u_aab = (D^3_+ - D^3_- - 2|b|^3 u_bbb)/(6|a|^2|b|)`, `u_abb = (D^3_+ + D^3_- - 2|a|^3 u_aaa)/(6|a||b|^2)`; best 1.8e-7..3.8e-6 at h = 1e-3 (S11).
* Count: the full symmetric order-k tensor for P parameters needs C(P+k-1, k) directions (P(P+1)/2 for the order-2 Hessian with the pure+sum form). Each direction is an independent lockstep family, so families parallelize across licences; the tensor-product "corner" grid (4 joint branches per pair, coupled to the pure families at every increment) is strictly worse for scheduling and not cheaper.

## 8. Recommended algorithm (implementable as specified)

```
INPUT  nominal run: accepted times t_0..t_N, u*_i (double), K_i (verified), force scale F
       direction d (parameter vector perturbation per unit t), order K, step h (relative)
       stencil S_k (offset list) for k = 1..K   [k<=2: {-1,0,1}; k=3,4: {-2,-1,0,1,2}]
       M = union of nonzero offsets of S_1..S_K
STATE  replay branch Z (P0) shared by all directions; branch B_m for each m in M (P0 + m h d)
       each branch = restartable Abaqus state after increment i-1 (or replay recipe)

for i = 1..N:
    ubar_i = u*_i with prescribed DOFs replaced by exact deck values at P0
    g0  = r_F( Z, i, ubar_i )                   # 1 run, shared; also logged as accuracy floor
    commit Z at ubar_i                           # same run
    factor K_FF,i once (reuse for all k, all directions, all iterations)
    c[i][0] = ubar_i
    for k = 1..K:
        est = 0                                   # free-DOF estimate of c[i][k]
        repeat (it = 0..maxit):
            for m in S_k, m != 0:
                x = sum_{j<k} (m h)^j / j! * c[i][j] + (m h)^k / k! * est    (free DOFs)
                x_C = exact prescribed values at P0 + m h d, time t_i
                g_m = r_F( B_m, i, x )            # 1 run: restart B_m at i-1, one increment,
                                                  #  all DOFs prescribed, double-precision RF
            g_0 = g0
            ck   = sum_m w_{k,m} g_m / h^k        # exact stencil weights
            delta = k! * K_FF^{-1} ck ; est += delta
            if k == 1 and it == 0 and Asym_i <= max(1e-2, 30 h): break   # smooth: accept
            if k > 1 and it == 0: break
            if ||delta|| <= 1e-8 * max(||est||, max_hist||c[.][k]||): break
        c[i][k] = est ; c[i][k]_C = exact BC Taylor coefficient
        record iterations, final ||delta||
    for m in M:                                   # commits, 1 run each
        x = sum_{j<=K} (m h)^j / j! c[i][j]  (free), exact BC at P0 + m h d
        rc_m = r_F( B_m, i, x ); commit B_m at x
    gates: Asym_i, predictor defect R_i = max_m ||rc_m - g0|| / (h max_hist||K c^(1)||),
           iteration convergence, g0/F
OUTPUT c[i][k] / (d_p)^k for pure directions; polarization for mixed (section 7)
```
Output-field sensitivities (S, PEEQ, SDV) are the same stencil applied to the field values of the **commit** runs (they are the predicted perturbed states), with offset 0 from the replay run — not implemented in this prototype (displacements only).

## 9. Cost (Abaqus one-increment runs per increment, per direction)

| scheme | branches | probes | commits | total | + shared replay |
|---|---|---|---|---|---|
| K=1 forward (spec, preview only) | 1 | 1 | 1 | 2 | 1 |
| K=1 central (default) | 2 | 2 | 2 | 4 | 1 |
| K=2 central | 2 | 2+2 | 2 | 6 | 1 |
| K=3 central | 4 | 2+2+4 | 4 | 12 | 1 |
| K=3 with O(h^4) orders 1-2 | 4 | 4+4+4 | 4 | 16 | 1 |
| K=4 central | 4 | 2+2+4+4 | 4 | 16 | 1 |

Adaptive re-probing adds one order-1 probe set (2 runs) per iteration only at flagged increments. Parameters/directions multiply linearly (mixed: section 7). If per-increment restarts are not reliable, a complete-history replay per probe costs O(N^2) increments per branch instead of O(N).

## 10. Default h policy and reliability gate

Steps are relative to |a| (or a declared scale when |a| is small or zero), and are clipped so a ± 2h stays within bounds.

| output precision of RF/U | order 1 | order 2 | order 3 |
|---|---|---|---|
| double (verified, recommended) | h_rel = 1e-5 (K=1); 1e-4 if K=2 shares branches | 1e-4 (window 1e-4..1e-3) | 1e-3, orders 1-2 from the same branches with O(h^4) stencils |
| float32 residual stored directly | 1e-4 | 1e-3 (experimental) | disabled |
| float32 with cancellation / unknown | 4e-3 (preview, label "low precision") | disabled | disabled |

Gate (per increment, per direction); label **validated** only if all pass, else show the value with a warning or mark unavailable:
1. Order-1 asymmetry `A_i <= max(1e-2, 30 h)` after any re-probing (smooth runs showed <= 1.8 h).
2. Re-probe iteration converged (||delta||/||est|| <= 1e-8 within 8).
3. Predictor defect `R_i <= max(1e-3, 100 h^2)` (K >= 2; exercised in S15) or `<= max(1e-3, 30 h)` (K = 1; proposed, not exercised), computed with g0 subtracted.
4. Nominal floor `||g0|| / F <= 1e-6`; otherwise cap the claimed accuracy at ~10 ||g0||/F.
5. Operator check `||K_op v - K_fd v|| <= 1e-6 ||K_fd v||` at sampled increments; otherwise iteration mandatory and reported.
6. Order 2: "validated" additionally requires double outputs and a free self-consistency check: with branches ±1, ±2 available compare the 3-point estimates at h and 2h (agreement <= 1e-4 relative over the history), else "experimental".
7. Order 3: **experimental by default** (narrow h window, 1e-6..4e-6 best case in double on 2-3 DOF problems, no Abaqus evidence); disabled without double outputs.

## 11. Reproduction

Environment used: Windows 11, Python 3.12.14 (uv-managed venv), numpy 2.5.4, mpmath 1.4.1. Runtime ~5 s for all studies.

```powershell
uv venv <dir>\venv --python 3.12
uv pip install --python <dir>\venv\Scripts\python.exe numpy mpmath
cd validation\reference_models\gauss
<dir>\venv\Scripts\python.exe run_studies.py            # all studies -> outputs\S0..S15.txt
<dir>\venv\Scripts\python.exe run_studies.py S4 S15     # selected studies
```

| study | content |
|---|---|
| S0 | tangent vs frozen-history FD of the oracle; cond(K) |
| S1-S3 | full h sweeps, orders 1-3, recursion vs full-rerun FD vs reference, predictor slopes, asymmetry (models A, B, C) |
| S4 | yield activation 1e-5 from / exactly at nominal; gates; spec probe vs iterated probe |
| S5 | stencil comparison incl. baked stencils |
| S6 | history ablation (own branch / frozen / shared) |
| S7 | float32 output emulations |
| S8 | nominal Newton residual and g0 handling, incl. the spec's one-sided form |
| S9 | cutback in a full-rerun reference |
| S10 | inexact K and defect correction |
| S11 | mixed derivatives via directional polarization, orders 2 and 3 |
| S12 | probe seeding variants |
| S13 | constrained-DOF treatment: exact BC in probe vs K_FC form |
| S14 | K=3 predictor slope vs order-1 stencil accuracy |
| S15 | recommended adaptive probing + gates |

## 12. Dissent from the spec

1. Section 3 item 3, one-sided `p_i = [r(..., a+h) - r(..., a)]/h`: O(h) truncation (2e-6..9e-6 at h = 1e-5, S5) and catastrophic if `r(u*, a)` is assumed zero (S8). Default should be central (±h, two branches) with the a-term measured by a replay probe.
2. Section 3 item 3, probe at the nominal current displacement: valid identity, but the probe state can cross yield surfaces the perturbed solution never reaches (errors 0.1-0.17 where full FD is exact, S3/S4). Add the adaptive re-probe at `u* + t s` (section 6.3). Without it, the in-app gate must flag these increments (it does).
3. Section 3 item 4, `K_FF s_F = p_F - K_FC s_C`: correct at order 1 (S13) but needs K_FC and does not extend cleanly; prefer exact perturbed BC values inside every probe/commit.
4. Higher orders: "a single branch at a+h with all lower-order predictors" is insufficient for any stencil other than the forward one; each offset is a full branch, and only structural stencils converge. The spec's "predictor residual O(h^2)" gate generalizes to slope min(K+1, 3) for central stencils, not K+1.
5. The order-3 claim should stay "experimental" even after Abaqus tests unless double-precision outputs and the self-consistency check of gate 6 pass on the real oracle.

## 13. Limitations and shared assumptions (what this does NOT show)

* All oracles are 2-3 DOF Python models with exact consistent tangents and no Newton noise inside the oracle (closed-form return map). Real UMAT local iterations add noise to every probe; real K has cond 1e5+.
* The reference for B and C is a full-rerun FD of the **same model code** (in 80-digit arithmetic). It shares the model definition and the definition of "discrete increment map" with the recursion; it does not share the recursion, stencils, or branch logic. Model A has a closed form. An independent check that breaks the shared-model dependence would be an independent implementation of model C (e.g. by Vera) or the Abaqus T2D2 truss/plastic bar against analytic results.
* The float32 study emulates rounding of RF/U only; the actual Abaqus RF semantics with loads at held DOFs, MPCs, contact and the coverage of `output_precision=full` are Curie's/Scout's open items.
* Restart/replay fidelity (identical history, same time grid, perturbed parameters from the branch start) is assumed, not tested.
* Displacements only; stress/SDV/derived-invariant sensitivities and geometry parameters are not prototyped.
* Gate thresholds were tuned on these three models; they are starting values to be re-calibrated on Abaqus cases.

## 14. Candidate lessons (proposed, not adopted)

1. In history-dependent residual-FD, the frozen-current-displacement probe can change the active set even when the true perturbed path does not; check one-sided asymmetry and re-probe at the predicted state.
2. Every stencil offset is a separate complete history branch; branches of one direction advance in lockstep, directions are independent.
3. The zero-offset residual must be measured on a replay of the exact prescribed values used by the probes; mixing deck-exact and file-read prescribed values silently destroys order-2 results.
4. Predictor-residual slopes saturate at min(K+1, 1 + p_1, 2 + p_2, ...); a slope of 3 at K = 3 is expected with O(h^2) lower-order stencils, not a failure.
5. Only stencils that annihilate all lower powers by their own weights converge for k >= 2 in this recursion.
