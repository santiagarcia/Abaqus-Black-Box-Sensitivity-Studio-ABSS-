# Council review record

- **Council.** The user's local Council at `C:\Users\vfn333\Documents\GitHub\The-council`, branch
  `feature/local-council`. Project `abss` (created with `council init-project`; objective set with
  `council objective set`).
- **Dispatch snapshot.** `1d52a2105ab2…` (`council dispatch --all --project abss`).
- **How members ran.** Each member ran as a separate Claude subagent and received its own Council packet (identity,
  system prompt, beliefs, shared objective). Builders and reviewers were separate invocations.
- **Independence.** Agent separation is organizational. It reduces anchoring but is not proof of independence
  (Council charter).

| Member | Role in this project | Artifact |
|---|---|---|
| Curie | Computational mechanics; Abaqus oracle and history experiments (**independent review #1** of the residual strategy) | [council/curie_review.md](council/curie_review.md), `validation/experiments/curie/` |
| Gauss | Numerics; recursion, stencils, step policy, risks (**independent review #2**) | [council/gauss_review.md](council/gauss_review.md), `validation/reference_models/gauss/` |
| Noether | Mathematical derivation (Theorems 1–2, constraints, polarization, outputs) | [council/noether_derivation.md](council/noether_derivation.md), `validation/reference_models/noether/` |
| Scout | Abaqus 2024 capability verification (tangent output, restart, precision, ODB API) | [council/scout_abaqus2024.md](council/scout_abaqus2024.md), `validation/experiments/scout/` |
| Maya | UX inventory of the reference viewer and native UX specification | [council/maya_ux_spec.md](council/maya_ux_spec.md) |
| Ada (×2) | Builders: INP parser/discovery/patching and example decks; Qt/VTK GUI | `src/inp`, `src/core/{mesh,parameter}.cpp`, `src/viz`, `src/gui`, `apps/abss` |
| Vera | **Independent reviewer** of the engine, validation gate and Abaqus oracle (built none of it) | [council/vera_review.md](council/vera_review.md), `validation/reference_models/vera/` |
| Main session | Integrator; core types, engine, synthetic oracle, Abaqus oracle, linalg, CLI, docs | the remaining files |

Atlas, Iris and Nico were not invoked. The integration decisions below were made by the main session, which is the
role Atlas would have played. Nico's cold-read comprehension review of the README was not run. That gap is recorded
here and was not filled in by pretending the review happened.

## 1. The two independent reviews of the residual / reaction-force strategy

Curie and Gauss worked separately and did not read each other's outputs. Noether derived the theory, also
separately.

| Question | Curie (Abaqus experiments) | Gauss (manufactured oracles, 80-digit references) | Noether (proof + sympy) | Decision |
|---|---|---|---|---|
| Is `r = −RF` at held DOFs? | **Measured**: rel 3e-16 including CLOADs on constrained DOFs and DLOAD pressure; the alternatives are falsified by the size of the load | assumed | hypothesis H-RF | **Adopted** |
| Probe with all DOFs prescribed, one increment | jobs with 0 equations run; replay (amplitude or step) and restart give the same RF to 4e-12 | – | Prop. 3 | **Adopted** |
| History mechanism | restart and restart chains reproduce S/PEEQ to 1e-12/1e-17; needs the `.sim` file. Scout: bit-exact | each offset is its own full-history branch (S6: shared history gives errors up to 5e4) | order-K history required (counterexample: 1.5–2.3%) | **Adopted**: per-offset restart chains from t=0; lockstep |
| Baseline `r(u*, nominal)` | free-DOF RF equals the nominal Newton residual (≤3.5e-7) | assuming 0 gives a 0.25 error (S8) | must be the measured h=0 replay | **Adopted**: nominal replay branch |
| Constrained DOFs | – | exact BC in probe is equivalent to K_FC form (7e-10) | Form A ≡ Form B (1e-59) | **Adopted**: Form B (perturbed BC values in every probe) |
| Tangent from Abaqus | `*MATRIX GENERATE` = elastic K at plastic states (113–127% off) | a 2% error in K gives 7–47% error in s | defect correction converges (§4.7) | **Adopted**: EMO K + tangent check + matrix-free GMRES (beyond the Richardson defect correction, because elastic-vs-plastic is not a small perturbation) |
| Off-path probes | probe linearity at plastic increments differs by 3e-3 between δ and δ/10 | **dissent with the spec**: a frozen-u* probe can cross yield surfaces the true path never reaches (S3/S4, 0.10–0.17) | – | **Adopted**: asymmetry gate + adaptive re-probe |

### Recorded dissent
- **Gauss vs. spec §3.3** (one-sided `p` with `r(u*, a)` taken as 0). Accepted: ABSS uses central branches and the
  measured baseline.
- **Gauss vs. spec item 3** (probe only at u*). Accepted: adaptive re-probe at u* + m h s.
- **Gauss: the predictor-residual slope saturates at min(K+1, 3).** Accepted; SCIENCE §3 now states O(h^min(K,2))
  relative to the first-order load.
- **Maya: notation `d` (total) vs. spec example `∂U2/∂E`.** Accepted: `d` for pure total derivatives, `∂` for mixed
  derivatives.
- **Scout: the open choice for plastic increments** was between flagging them Experimental and a matrix-free Krylov
  solve. Decision: Krylov (implemented and validated synthetically). Flagging remains the fallback when GMRES does
  not converge, which is reported as an engine issue.

## 2. Higher-order and history recursion
- Noether Theorem 2 (order-k recursion with the Taylor predictor and an order-K-complete history) was confirmed to
  1e-60 on a 2-DOF history model. It was confirmed independently by Gauss on three manufactured models with
  80-digit references:
  - orders 1–3;
  - O(h⁴) stencils for orders 1–2 when M = 2;
  - mixed derivatives by polarization over independent directions.
- In ABSS the same machinery serves all orders from one set of branches. Commits use the order-K predictor.

## 3. Decisive experiments and their outcomes (on this machine)
| Gate (spec §7) | Experiment | Outcome |
|---|---|---|
| Operator = frozen-history directional derivative | EMO K vs. oracle FD: CPS4 linear 8.5e-13; NLGEOM 1.06e-7 (Scout); ABSS tangent check on the truss 3.1e-12 | **pass** for elastic/NLGEOM-elastic; **fails at plastic states**, which is handled by Krylov |
| Frozen-displacement RF = free-DOF imbalance | Curie E1 (3e-16, 1.8e-15) | **pass** |
| Restart/replay preserves history and time schedule | Curie E2/E2c/E2d, Scout replay | **pass** (1e-12; time grid identical) |
| Predictor residual O(h²) (order 1, smooth) | ABSS predictor-consistency diagnostic; measured O(h) relative to the first-order load, i.e. O(h²) absolute | **pass** (synthetic); recorded per run |
| End-to-end plastic first-order sensitivity on Abaqus (independent pipeline) | Curie E5: CPS4 J2, 10 plastic increments, 140 jobs | **pass**: 6e-5 to 1.1e-4 per increment vs. converged central FD |
| Full reruns agree at each increment as h shrinks | synthetic: per-increment for all fields, plus h sweeps (VALIDATION_RESULTS §2). Abaqus: truss, J2 CPS4 (§3) | synthetic **pass**. Abaqus: single h per run (h-shrink sweeps on Abaqus are not done; see open items) |

## 4. Independent review of the implementation (Vera) and the response
Vera's verdict was **changes requested**, with two blockers. Every finding and what was done:

| # | Finding | Response | Evidence |
|---|---|---|---|
| F1 | Cantilever `L_beam` (smooth) wrong by O(1) with almost no flag; the builder's "straddle" explanation was wrong | **Fixed.** Re-probe threshold `max(1e-2, 30 h_rel)` (Gauss S15), up to 8 passes, 1e-8 convergence. Wrong explanation and hand-tuned test step removed. | `test_review_regressions.cpp` F1: 9.6e-4 / 1.2e-3 at default steps (was 2.1 / 8.6) |
| F2 | "Validated" with nothing compared (NaN, missing frames, empty filters) | **Fixed.** Validation counts compared values; NaN/Inf where data exists → Failed; missing increments → Failed; nothing compared → Unverified. | three regression tests (E1/E2 probes + empty filter) |
| F3 | False FAIL on the Abaqus truss `dS/dE` (true value 0) | **Fixed in two ways.** The metric floor is now 1e-4 of the natural scale for orders 1–2 (so 5e-12 counts as zero), and Abaqus reruns now use `*CONTROLS, PARAMETERS=FIELD` R_n = 1e-8. The A3 FAIL is disclosed in VALIDATION_RESULTS. | VALIDATION_RESULTS §3 |
| F4 | The detector misses inherited errors; thresholds arbitrary; one criterion dead | **Fixed.** Checks all branches; derived threshold 50·h^p (measured smooth C ≈ 2); dead criterion removed; the evidence note states that later increments inherit; engine failures are separated. | regression: every wrong increment is flagged or inherits from a flagged one |
| F5 | GMRES: no convergence status; tolerance below the FD-operator noise; failures shown as "non-smooth" | **Fixed.** `krylovConverged`, tolerance 1e-8, reported as an `ENGINE ISSUE` | engine notes |
| F6 | `*CLOAD, FOLLOWER` silently fixed; self-check one-directional; no evaluator tests | **Fixed.** FOLLOWER refused; CF check two-directional; 4 unit tests for the evaluator. DLOAD/DSLOAD values are still not self-checkable (CF excludes them), which is documented. | `tests/oracle/test_loads.cpp` |
| F7 | Concurrent Abaqus jobs share one branch directory | **Measured, not changed.** The 20-increment J2 run used 6 concurrent jobs with distinct job names; outcome in VALIDATION_RESULTS §3 | – |
| F8 | Metric normalises by field max | **Kept, documented.** Per-component errors are not separately bounded (KNOWN_LIMITATIONS). | – |
| F9 | Docs cite a missing VALIDATION_RESULTS, A3 FAIL undisclosed, wrong SCIENCE scaling | **Fixed.** | docs |
| F10 | Completion check used a substring | **Fixed.** Exit code, exact "Abaqus JOB <name> COMPLETED", timeout | `abaqus_oracle.cpp` |

A second Vera pass on the fixes was not run. Under Council rules the fixes are therefore **not independently
re-certified**. They are covered by the regression tests Vera specified.

## 5. Open items carried forward
- **Pending Abaqus tests:** UMAT/UEL (needs Intel Fortran), constraints, contact, shells. See
  ABAQUS_REQUIRED_TESTS.
- **Gate not met on Abaqus:** h-shrink sweeps on real Abaqus models (spec §7, "as h shrinks").
- **Not yet reviewed:** a Nico cold-read of the README, and a second Vera pass on the fixes.
