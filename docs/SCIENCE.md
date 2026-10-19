# ABSS science: history-consistent sensitivities from an opaque Abaqus oracle

This document states exactly what ABSS computes, under which assumptions, and how each claim is checked.
Proofs and high-precision symbolic checks are in [council/noether_derivation.md](council/noether_derivation.md).
The independent numerical review is in [council/gauss_review.md](council/gauss_review.md). The Abaqus 2024 facts
come from real runs: [council/scout_abaqus2024.md](council/scout_abaqus2024.md) and
[council/curie_review.md](council/curie_review.md). Measured results are in [VALIDATION_RESULTS.md](VALIDATION_RESULTS.md).

## 1. Problem statement and sign conventions

- **Grid.** Accepted increments are `i = 1..n` on the **frozen nominal time grid** `t_0 < t_1 < … < t_n`.
  Derivatives are derivatives of the discrete algorithm on this grid.
- **Residual.** `r_i(u_i, u_{i-1}, q_{i-1}, a) = F_ext,i − F_int,i`, where `q` is the opaque history Abaqus carries
  (stresses, SDVs, plastic strains, …).
- **Tangent.** `K_i = −∂r_i/∂u_i` (positive stiffness).
- **Reactions.** `RF = F_int − F_ext = −r` on every DOF Abaqus holds fixed.
  - Verified on Abaqus 2024 for T2D2/CPS4, with CLOADs on held DOFs (Scout: replay is bit-exact; Curie: E1).
- **Sensitivities.**
  - `s_i = du_i/da`, `u_i^(k) = d^k u_i/da^k`: total derivatives along the equilibrium/history branch.
  - Labels `dU2/dE`, `d²S_Mises/dE²` denote total derivatives. Mixed derivatives use `∂²/∂a∂b`.

## 2. First order

Differentiating `r_i ≡ 0` along the branch gives

    K_i s_i = p_i,   p_i = ∂_a r_i + ∂_{u_{i−1}} r_i s_{i−1} + ∂_{q_{i−1}} r_i dq_{i−1}/da     (history-carrying pseudoload)

ABSS never differentiates the constitutive update. It obtains all of `p_i` from oracle differences:
- A **history branch** at `a + m h` is advanced from `t = 0` by *prescribing* `u_j = u_j* + Σ_l (mh)^l/l! u_j^(l)` at
  every earlier increment, so its history is correct to `O(h^{K+1})`.
- A **probe** at increment `i` evaluates `r_F` with the current displacement frozen at the predictor while the history
  stays untouched (Noether Thm. 1).
- The **nominal replay branch** (`m = 0`) supplies the measured baseline `r_i(u_i*, nominal history)`. It is never
  assumed to be 0. Abaqus converges only to a tolerance; assuming 0 adds an error of order tol/h (Gauss S8: 0.25 error).

**Form B (production).** In every probe the originally constrained DOFs take their *perturbed* prescribed values
`g_i(a + m h)`. Then `K_FF s_F = D_h r_F` with no `K_FC` block. Parameter-dependent BCs (`s_C ≠ 0`) are included
exactly, and `s_C` comes out of the commit stencil. Form A (`K_FF s_F = p_F − K_FC s_C`) agrees to 1e-59 in the
symbolic check and is kept as theory only.

## 3. Higher order (pure) — Theorem 2

With the order-`k` predictor `U^[<k](h) = u* + Σ_{l<k} h^l/l! u^(l)` and branches committed with the order-`K` predictor:

    K_FF u_F^(k) = k! [h^k] r_F(U^[<k](h), g(a+h), branch history, a+h),     [h^j] r_F = 0 for 1 ≤ j < k.

- `[h^k]` is extracted with central Fornberg stencils over offsets `m ∈ {−M..M}`, with `M = 1` for `K ≤ 2` and
  `M = 2` for `K = 3, 4`.
  - The 5-point set gives `O(h^4)` for orders 1–2 and `O(h^2)` for order 3 (Gauss S14, Noether D).
- Each offset is its own complete history branch. All branches of one direction advance in lockstep (each increment,
  then each order).
  - Committing with fewer than `K` orders gives `O(1)` errors at order `K`. Noether counterexample: 1.5–2.3%.
  - Frozen or shared history gives errors of 0.22–5e4 (Gauss S6).
- **Diagnostics recorded per increment:**
  - `[h^{K−1}] r(U^[<K])` ("lower-order leak"; should vanish).
  - The commit residual `|r_F(U^[≤K](mh)) − r0|` on every branch, divided by `|m|` times the first-order load
    `|D¹r|`: smooth `≈ C (|m| h)^{min(K,2)}` with measured `C ≈ 1.6–2`. That is `O(h)` relative (`O(h²)` absolute)
    for K = 1. It saturates at `min(K,2)` because of the `O(h²)` order-1 stencils (Gauss dissent 5).
  - The order-1 asymmetry `|r(+h) + r(−h) − 2 r0| / |r(+h) − r(−h)|`: smooth `O(h)`, kink `O(1)`.

**Mixed derivatives** come from polarization over independent directions:
`∂²u/∂a∂b = (D²_{a+b} u − D²_{a−b} u) / (4 h_a h_b)`, where each direction is its own branch family (Gauss S11).
They are validated against four-corner full reruns.

## 4. Output fields

S, E/LE, PEEQ, SDV, RF and invariants are evaluated on the **commit** branches. They are **never** evaluated on
probes, which would give only a partial derivative. The same stencil is used:
`d^k y/da^k = D^k_h y(commit branches)`.

- **Invariants (Mises).** The invariant is evaluated per branch and then differentiated. This is the derivative of
  the invariant, not the invariant of a tensor derivative (unit test: `test_3d_and_io.cpp`).
- **Non-differentiable points.** At deviatoric-zero points and at yield onset the derivative does not exist. These
  points are flagged (§6).
- **Storage.** Integration and section-point values are never averaged in storage. The GUI may display an element
  average, labelled as a display projection.

## 5. Geometry parameters

Branch decks move reference coordinates `X(a+mh)` through token edits. Node/element identities and connectivity are
unchanged, and topology changes are refused. Probes freeze `u` (not `x`), so `∂_X r X'` (stiffness, load area and
direction) enters the pseudoload automatically. ABSS reports `du/da`. BCs are interpreted as displacements; a BC that
fixes an absolute position would need `g' = −X'_C` (not implemented; documented limitation).

## 6. Non-smoothness, step size and noise

Plasticity, contact and similar effects make responses piecewise smooth.

**Evidence of the step-size trade-off** (synthetic J2 plate, all fields and increments, VALIDATION_RESULTS §2):
- At `h/a ≥ 1e-3` the stencil straddles yield onsets, and the order-1 error is 0.03–0.8 for engine and reference alike.
- At `h/a = 1e-4`, order 1 is 6e-8 to 9e-7 and order 2 is about 2e-6.
- At `1e-5`, order 1 is 6e-10 (clean `O(h²)`). Order 2 is limited by the reference noise ~ tol/h².

**Default steps** (`defaultRelativeStep`): `h/|a| = 1e-5, 1e-4, 1e-3` for `K = 1, 2, ≥3`. A zero nominal uses a
bound-based scale.

**Detection (no smoothing).** An increment is flagged as a *predictor inconsistency* when the commit-residual ratio
(§3) exceeds `max(50 h_rel^{min(K,2)}, 1e-6)`, which is a 25× margin over the measured smooth constant. The flag is
shown in the GUI and the evidence, and it states that later increments inherit the history error. The cause is either
a genuine non-smooth event or a probe that left the true path. **Engine failures** (re-probe or GMRES not converging)
are reported separately as `ENGINE ISSUE`, never as physics. Regression: with re-probing disabled, every increment
with an error above 1e-3 is flagged or follows a flagged one (`test_review_regressions.cpp`).

**Off-path probes** (Gauss S3/S4): a frozen-`u*` probe can cross a yield surface the true perturbed path never
reaches. When the order-1 asymmetry exceeds `max(1e-2, 30 h_rel)` (smooth ≈ 1.8 h_rel), ABSS re-probes at
`u* + m h s` until the update is below 1e-8 relative (at most 8 passes; Gauss S15, Noether §4.7). Example: on the
cantilever length parameter the error at the default step went from 2.1 to 9.6e-4 (Vera F1).

**Precision:**
- All prescribed displacements are written with round-trip 17 significant digits. At 7 digits, order 2 has an
  error of about 1.6e-2 (Noether F).
- Results are read from the ASCII `.fil`, which has 16 significant digits; the ODB is float32 by default.

## 7. The tangent

- **Source.** `K_i` comes from `*ELEMENT MATRIX OUTPUT` at every converged increment, assembled by ABSS. BCs are not
  applied in that output; ABSS extracts `K_FF`.
  - Verified for elastic CPS4 (1.1e-14 vs. analytic), NLGEOM-elastic CPS4/T2D2 (1e-7 vs. FD), and the T2D2 example
    (tangent check 3e-12).
- **Under built-in Mises plasticity this K is the elastic-modulus stiffness, not the consistent tangent** (51–76%
  off, Scout).
- **Remedy, implemented and tested.**
  - Every increment runs a tangent check: `K v` against the central FD of two probes on the replay branch.
  - If the check exceeds 1e-5, the solves become **matrix-free GMRES**. The operator is
    `K_true v = −[r(u*+εv) − r(u*−εv)]/(2ε)`, using probes with frozen history, and the factorized Abaqus K is the
    preconditioner.
  - On the synthetic J2 plate with a deliberately elastic tangent, this recovers validated order 1–2 results. A
    negative control without Krylov fails (`test_krylov.cpp`).
  - Cost: two Abaqus probe jobs per operator application.

## 8. The Abaqus oracle (how the residual is obtained)

1. **Nominal job.** The user's deck is copied, never edited, with these additions:
   - `*FILE FORMAT, ASCII`
   - `*NODE FILE U, RF, CF`
   - `*EL FILE S, E|LE, PE[, SDV]`
   - `*ELEMENT MATRIX OUTPUT ... FREQUENCY=1`

   It defines the frozen grid. A **self-check** re-evaluates every BC and concentrated load from the deck semantics
   and compares them with the nominal output. A mismatch above 1e-6 aborts the run.
2. **Branch at `a+δ`.** The token-edited deck for that branch is run from `t = 0`, because Abaqus silently ignores
   material edits on restart. Increment `i` is a single-increment restart job from the branch's increment `i−1`:

        *RESTART, READ, STEP=i−1, INC=1, END STEP
        *STEP, NLGEOM=…, INC=1 ; *STATIC, DIRECT ; Δt_i, Δt_i
        *BOUNDARY, OP=NEW   (every independent DOF: free at the requested u, constrained at g_i(a+δ))
        *CLOAD/*DLOAD/*DSLOAD, OP=NEW at their t_i values
        *NODE FILE U, RF  (+ element output for commits)

   `r = −RF`.
   - **Probes** set `*RESTART, WRITE, FREQUENCY=0` and are discarded.
   - **Commits** write restart data and become the history.
3. **Gates checked on every job:**
   - The job must complete with no `***ERROR`.
   - There must be exactly one increment at the expected total time (`H-ONE-INC`).
   - The `.dat` must not contain "KEYWORD IS IGNORED IN A RESTART".
4. **Validation reruns.** These are full re-equilibrated jobs on the same grid (`*STATIC, DIRECT`). They need a
   uniform increment per step; otherwise validation is reported as not possible rather than faked.

## 9. Validation gates (what "Validated" means)

A derivative series is **Validated** only after it has been compared, at **every** increment and for **every**
field, against the central FD of full re-equilibrated reruns at `a ± m h`.
- **Error metric.** Max absolute difference divided by `max(|reference|, floor_k · max|F_nominal| / |a|^k)`.
  - `floor = 1e-4` for orders 1–2 and `1e-3` for orders ≥3. Below that, the change to the Taylor term is negligible.
- **Tolerance.** 1e-3 (order 1) or 1e-2 (order 2) by default; the tests use 1e-4 and 1e-3.
- **Orders ≥ 3.** These are labelled **Experimental** even when they pass.
- **Mixed.** Validated against four-corner reruns.
- **Otherwise.** The series is **Unverified** (no reruns) or **Failed**. Synthetic demo results carry a separate
  "synthetic" banner.

## 10. Limitations (summary; full list in KNOWN_LIMITATIONS.md)

- Quasi-static `*STATIC` only.
- Flat decks only (no parts/instances in the Abaqus backend).
- Not supported: constraints (MPC/TIE/coupling/equations), contact, nonlinear MPCs, dynamics, multiphysics,
  RIKS.
- Changing constrained-DOF sets between steps is not supported.
- UMAT/UEL are opaque: only exposed PROPS are differentiable. They are untested on this machine (no Intel Fortran).
- The derivative is that of the algorithm on the frozen grid. Cutbacks in perturbed reruns make validation
  impossible, and the run reports that rather than producing results.
- Bifurcation and limit points (singular `K_FF`) are not handled beyond solver failure messages.
