# Vera: independent review of the ABSS sensitivity engine, validation gate and Abaqus oracle

- Owner: Vera (independent verification). Date: 2026-10-16. Mode: verification. The affective layer was enabled, but
  it did not change any evidence threshold.
- Council snapshot: `1d52a2105ab2…` (project `abss`). Builder: main session (Ada role). Reviewed working tree: `main`,
  clean at session start, no commits.
- **Independence.** I did not write, design or direct any of the reviewed code or documents. I changed no product
  code. My only files are this review and the probes in `validation/reference_models/vera/`
  (`vera_probe.cpp`, `build_and_run.ps1`). The probes call the product engine, validation and synthetic oracle, so
  they are not an independent implementation of the physics. Three parts are independent of the builder's code:
  - the per-frame error metric;
  - the step-dependence ("kink") reference: central differences of full reruns at `h` and at `h/10`;
  - the decoding of the real-Abaqus project.
- No Abaqus job was run by me. I read the builder's real-Abaqus scratch outputs (`abq_truss`, `abq_j2`) read-only.

## Verdict

**Requested changes. The engine is not accepted as validated for the general claim yet.**

The mathematics in `engine.cpp` matches SCIENCE.md and Noether Theorems 1–2. The generated Abaqus decks match the
mechanism Scout verified. The fixed-step synthetic results that the tests report are reproducible.

However, there are two blocking problems, both reproduced below:

1. **The engine can return O(1)-wrong sensitivities while the true response is smooth.** The order-2 error was 860%.
   At default settings these results carry no adequate warning: only 1 of about 6 bad increments is flagged, and
   that flag misnames the cause as a "non-smooth event".
2. **The validation gate can assign "Validated" without any successful comparison.**

## Ranked findings

Rank = severity × likelihood × consequence. File:line refers to the reviewed tree.

### F1. Blocker: silent O(1) error from off-path probes; re-probe gate looser than the reviewed recommendation

- **Where:**
  - `include/abss/sensitivity/engine.hpp:42` (`asymmetryThreshold = 0.05`)
  - `src/sensitivity/engine.cpp:388-410`: re-probe only if asymmetry > 0.05; max 3 passes; "converged" at 1e-6
- **Deviation from the reviewed design.** Gauss S15 (`gauss_review.md:128,219`) recommended:
  - re-probe when asymmetry > `max(1e-2, 30 h_rel)`;
  - correct until the change is ≤ 1e-8 (at most 8 passes);
  - gate the predictor defect at `max(1e-3, 100 h²)`.

  The implementation uses a fixed threshold that is 5× looser and does not implement that predictor-defect gate.
- **Reproduced (E3/E4/E6):** synthetic cantilever `cantileverBlock(6,2,2)`, parameter `L_beam`.
  - **Smooth reference.** At h/a = 1e-3, 3e-4 and 1e-4, full-rerun FD at `h` agrees with full-rerun FD at `h/10`
    to 2e-6–2e-8 at every increment. The response is smooth and the derivative exists.
  - **Engine vs FD of reruns at the same h:**

    | h/a | max error | flags |
    |---|---|---|
    | 1e-3 | 3.27 | none |
    | 3e-4 | 3.06 | none |
    | 1e-4 | 2.10 | frame 4 only |

    Frames 3–8 all carry errors of 0.13–3. Asymmetries are 0.02–0.045, which is below 0.05.
  - **Default step for K = 2** (h/a = 1e-4, no user step): order 1 error 2.1, order 2 error 8.6. One suspect frame
    is reported, and it is labelled "possible non-smooth event (yield onset…)". That is a misdiagnosis: the rerun
    response is smooth.
  - **Same run, different settings:**
    - `asymmetryThreshold = 0.01` → order 1 error 9.6e-4, order 2 error 1.2e-3 (both pass);
    - `0.001` → 3.2e-5, 1.3e-4;
    - 3 forced re-probe passes → 8e-6, 1.4e-4.
- **The test suite avoids this case rather than catching it.** `tests/sensitivity/test_3d_and_io.cpp:20-21` hand-sets
  `L_beam` to h/L = 3e-5. Its comment says h/L = 1e-4 "straddles a yield onset". My reruns contradict that: at
  1e-4 the FD is step-independent to 2e-8. The straddle explanation is unsupported. The engine is what fails.
- **Consequence.**
  - Without `--validate`, which is opt-in, a user gets 200–900% wrong derivatives labelled Unverified, with at most
    one frame flagged and the wrong cause stated.
  - With Abaqus the risk is higher. Real meshes have many more integration points near the yield surface, and the
    max-norm asymmetry dilutes local active-set flips.
- **Decisive test.**
  1. Add a ctest that runs the E6 configuration: cantilever, `L_beam`, K = 2, *default* step.
  2. It must either reach Validated, or flag **every** frame whose error vs reruns exceeds the tolerance.
  3. Also report the asymmetry distribution over a smooth h-sweep, so the threshold is justified by data and not
     chosen by hand.

### F2. Blocker: "Validated" can be assigned without any comparison (false PASS)

- **Where:** `src/sensitivity/validation.cpp:22-43, 52`.
- **NaN is silently ignored.**
  - The code uses `std::max(err, NaN)` and `std::max(scale, NaN)`. These return the first argument, so a NaN never
    propagates.
  - E1: every value of `dU/dE` set to NaN gives Validated (the error reported, 1e-10, comes from other fields). With
    `fields = {"U"}` it gives "max rel. error 0 [] PASS", which is Validated.
  - NaN occurs in practice. `AbaqusOracle::fieldsFrom` fills missing integration-point data with NaN
    (`abaqus_oracle.cpp:341,356`): no PE for a UMAT, a user `*EL FILE` position mismatch, or SDV absent. A singular
    GMRES Hessenberg diagonal (`engine.cpp:251`) would also produce NaN.
- **Missing series are skipped.**
  - `if (!s) continue;` (`validation.cpp:32`). E2: an engine run cancelled before increment 1 stores no series.
    Validation then reports "max rel. error 0 [] PASS" for orders 1 and 2, and both become **Validated**.
  - A partially cancelled run is validated on only the frames it has. The note still says "all increments".
- **A field filter that matches nothing** gives `worst = 0`, which also passes.
- **Spec conflict.** SCIENCE §9 says a series is Validated only after comparison at *every* increment for *every*
  field. The code does not enforce this.
- **Decisive test.** Assert Failed or Unverified when:
  - any compared value is non-finite;
  - any (field, frame) of the spec is missing;
  - zero values were compared.

  E1 and E2 in `vera_probe.cpp` are ready-made regression cases.

### F3. High: the validation reference is noisier than the engine; one confirmed false FAIL on real Abaqus; no h-sweep

- **Evidence (E0)** decodes the builder's real Abaqus 2024 project `scratchpad/abq_truss/truss.abss` (test A3).
  - **The truth.** For the T2D2 truss `S = F/A` is independent of E, so `dS/dE ≡ 0`.
  - **The engine** stores `dS/dE = 5.0e-12`.
  - **The reference.** Validation reports FAIL with an error of 0.01 against the floor scale 1e-4·100/2e5 = 5e-8.
    That implies a reference value of about 5e-10, i.e. `S(E+h) − S(E−h) ≈ 2e-8` (2e-10 relative).
  - **The cause.** That size is consistent with Abaqus's "CRITERION FOR RESIDUAL FORCE FOR A LINEAR INCREMENT 1e-8"
    (from `abss_nominal.msg`). The FAIL comes from rerun noise, not from the engine.
- **Where:**
  - `abaqus_oracle.cpp:585-654`: reruns get no `*CONTROLS` tightening.
  - Curie (`curie_review.md:139`) explicitly recommended `*CONTROLS, PARAMETERS=FIELD` for sensitivity runs.
  - Curie also needed `EXTRAPOLATION=NO` to get past an unloading kink (`:49`). ABSS does not set it.
- **Scaling.** For nonlinear increments the default R_n = 5e-3. The reference noise scales as tol/h, so at the K = 1
  default h/a = 1e-5 a FAIL can be pure reference noise, and a PASS cannot be trusted at the floor level either.
  The floor comment (`validation.cpp:25-26`) claims it sits above reference noise only for k ≥ 2.
- **Spec gate not met.** Master prompt §7 requires reruns that "agree … as h shrinks". Validation runs at one h
  (`stepFactor = 1`), with the same stencil and the same h as the engine. It therefore cannot see step-dependence.
  Noether §4.5 says "ABSS must run an h-sweep and report the plateau". KNOWN_LIMITATIONS admits this is missing,
  but the "Validated" label does not say so.
- **Decisive test.**
  1. Rerun A3 with `*CONTROLS, PARAMETERS=FIELD` tightened (e.g. R_n = 1e-10), and with stepFactor 1 and 10.
  2. Expect `dS/dE` to PASS and order 1 to agree across stepFactors.
  3. Add a reference-noise estimate to the metric: rerun the nominal twice through the two paths (replay vs rerun),
     or use the measured r0, and use it to floor the scale.

### F4. High: the non-smoothness detector has false negatives, dead criteria and misattribution

- **Where:** `engine.cpp:431-437, 493-508`.
- **Only the +h branch is checked.** The commit residual uses `br[d][bPlus]` alone. A straddle that only the −h
  branch sees, such as σy decreasing into yield, is checked only through asymmetry and re-probe.
- **Only the event frame is flagged.** The straddle is carried forward through history: E4 shows step-dependence of
  0.16–0.38 on frames 5–7 after an onset at frame 4. The detector flags only frame 4.
- **E4 cases (step-dependence of the full-rerun FD between h and h/10 > 1e-2, unflagged):**

  | case | h/a | unflagged frames | notes |
  |---|---|---|---|
  | cantilever σy | 1e-2 | 3, 4, 5, 6, 7 | 0 flags; step-dependence up to 0.38 |
  | cantilever σy | 3e-3 | 5, 6, 7, 8 | |
  | cantilever σy | 1e-3 | 5, 6, 7 | |
  | plate σy | 1e-2 | 2, 3 | |
  | plate E | 1e-2 | 2, 3, 10 | |
  | plate E | 3e-3 | 10 | |

  In these cases validation does FAIL, so a validated run is protected. An unvalidated run is not.
- **Dead criterion under defaults.** "Tangent check > 1e-3 without Krylov" can never fire, because any tangent-check
  error above `krylovTrigger = 1e-5` switches Krylov on.
- **Arbitrary thresholds.** "20× median and > 1e-2" has no derivation. Frames 5–7 of the F1 case have commit
  residuals of 2e-4–7e-3 with O(1) errors.
- **Misattribution.** Every flag is worded as a physical event. F1 and F5 show the same flags appear for engine
  errors: off-path probes and GMRES non-convergence.
- **Decisive test.** Use the E4 sweep (`build_and_run.ps1 <proj> sweep`) as a regression. Every frame with
  step-dependence > 1e-2 must be flagged, and the flag must name the criterion that fired.

### F5. Medium: GMRES has no convergence status, its target is below the operator noise, and failure is mislabelled

- **Where:** `engine.cpp:187-261`.
- **The algebra is correct.**
  - Right-preconditioned Arnoldi with modified Gram–Schmidt.
  - Givens rotations on column k with previous rotations applied first.
  - Back-substitution on the k×k triangle, and the update `x += Z y` using the stored preconditioned vectors.
  - Restarts recompute the true residual.
- **Problems:**
  1. A non-converged solve returns `x` with no failure status. Only `krylovResidual` is recorded, and the GUI/CLI
     shows it only as a note.
  2. The target `krylovTolerance = 1e-9` is below the FD-operator noise. The operator uses `eps = 1e-6·uScale`, so
     noise is about 1e-10 to 1e-8 relative, and more with Abaqus's internal tolerances. Stagnation can then run
     4 × 60 iterations × 2 Abaqus jobs per RHS per frame, which is hours at about 13 s per job.
  3. `H[j][j] = 0` gives inf/NaN (`:251`), which F2 then passes.
  4. `solveResidual` is not computed in Krylov mode, so the log prints "solve residual 0".
- **E5:** `krylovMaxIterations = 1` gives a residual of 4.6e-2. Status stays Unverified. Frames 3, 4 and 10 are
  flagged as "possible non-smooth event" rather than "linear solve failed". Validation FAILs (0.11).
- **Decisive test.**
  1. Make GMRES non-convergence a hard per-frame flag that names the solve.
  2. Derive the tolerance from a measured operator-noise estimate (repeat one matvec at eps and 2·eps).

### F6. Medium: load/BC replay — unsupported parameters are dropped silently, and the self-check is partial

- **Where:** `src/oracle/abaqus_loads.cpp` and `abaqus_oracle.cpp:460-481`.
- **`*CLOAD, FOLLOWER=YES` is silently replayed as a fixed-direction load.** Only `OP` and `AMPLITUDE` are read, and
  `stepText` writes `*CLOAD, OP=NEW` without the parameter. Under NLGEOM this changes the residual. The self-check
  compares CF magnitudes per DOF and may not catch a direction change.
- **Not refused:** `*BOUNDARY, USER`/`FIXED`, `*TRANSFORM`ed nodes, and `*AMPLITUDE, SMOOTH`. Each is either ignored
  or caught only incidentally by the BC check.
- **The self-check does not cover `*DLOAD`/`*DSLOAD` at all**, including amplitudes, OP=NEW and propagation.
- **The CLOAD check is one-directional.** It only compares DOFs the evaluator itself produced (`:470`). A CF that
  Abaqus applies at a DOF the evaluator omits is never compared.
- **Unverified semantics.** Two Abaqus behaviours are encoded as assumptions and verified only where the CF/BC check
  reaches:
  - OP=NEW redefinition of an existing load ramps from 0 (`abaqus_loads.cpp:174-175`);
  - a propagated STEP TIME amplitude holds its end value (`:135`, whose comment says "cross-checked").
- **No unit tests.** `tests/oracle/` does not exist, so `LoadEvaluator`, `readFil` and `readEmo` have no unit tests.
  The CMake module silently returns when it finds no sources.
- **Decisive test.**
  - Refuse unknown parameters on load/BC keywords.
  - Add unit tests of `LoadEvaluator` against hand-computed multi-step tables (MOD/NEW, ramp/step, propagated
    amplitude).
  - Run Abaqus test C2 to cover DLOAD with an independent output, such as a summed reaction balance.

### F7. Medium: concurrent Abaqus jobs share one directory and one restart file set

- **Where:** `AbaqusBranch::residuals` (`abaqus_oracle.cpp:724-739`) launches several jobs at once in the same
  `branch_N` directory. All of them use the same `oldjob` restart files. The GMRES operator does this on every
  matvec (`{up, um}`).
- **Not verified.** Scout ran sequentially with `cpus=1`. The in-progress j2 run shows simultaneous
  `b0_p4_42.simdir` and `b0_p4_43.simdir`. That shows it runs, not that the results are identical.
- **Two more collision risks:**
  - Branch directory names restart at `branch_0` per oracle instance, so two runs that share a scratch directory
    collide.
  - The Abaqus ctest uses a fixed `%TEMP%\abss_ctest_truss` with `remove_all`.
- **Decisive test.** Run one frame's probes with `maxConcurrentJobs = 1` and again with 4. Require bit-identical RF.
  Alternatively give each probe its own subdirectory with copies of (or links to) the restart files.

### F8. Low–medium: metric design can mask component errors and depends on units at zero nominal

- **Masking.**
  - The error is normalised by the maximum |reference| over all frames and components of a field
    (`validation.cpp:33-39`).
  - Small components can therefore be completely wrong, including the wrong sign, and still pass. At tol 1e-3 this
    applies to any component smaller than 1e-3 of the field maximum.
- **Units at zero nominal.**
  - A zero-nominal parameter with no finite bounds uses scale 1 (`parameter.cpp:27-31`) for both the step and the
    floor.
  - Both then depend on the user's units.
- **Recommendation.** Report a per-frame and per-component error distribution, not only the maximum. Require an
  explicit scale for zero-nominal parameters.

### F9. Low: documentation claims without evidence locators

- **The cited evidence file does not exist.** `docs/VALIDATION_RESULTS.md` is not in the repo. It is cited by:
  - SCIENCE.md §0 and §6 (the step-size numbers 6e-8, 9e-7, 6e-10, 0.03–0.8);
  - `engine.hpp:26`;
  - `synthetic_oracle.cpp:51`;
  - the `abaqus.hpp:43` and `abaqus_oracle.cpp:39` capability strings ("Verified … see VALIDATION_RESULTS");
  - ABAQUS_REQUIRED_TESTS A2–A4 ("see VALIDATION_RESULTS §3").

  These numbers are currently untraceable.
- **The A3 real-Abaqus run is not disclosed.** It produced a FAIL (F3), and no document reports it.
- **SCIENCE §3 is inconsistent with the code.** It says the commit residual relative to the first-order load is
  `O(h^{min(K,2)+1})`. The code comment (`engine.hpp:64`) says `~O(h^K)`. My derivation in t-units agrees with the
  code: the relative ratio is O(h) for K = 1 and O(h²) for K = 2. The measurements also agree with the code. In E4
  (cantilever σy, K = 1), frame 2 has a commit ratio of 4.87e-3, 1.46e-3 and 4.85e-4 at h/a = 1e-3, 3e-4 and 1e-4.
  That is linear in h, not O(h²). SCIENCE §3 is wrong.
- **Wrong keyword in ABAQUS_REQUIRED_TESTS C4.** It names "`*DLOAD, FOLLOWER`". In Abaqus, FOLLOWER is a `*CLOAD`
  parameter.
- **SCIENCE §6 overstates the detector.** It lists the detector as if it were effective. F1 and F4 show that it is
  not.

### F10. Low: process gate details

- **Where:** `runJob` (`abaqus_oracle.cpp:306`).
- **The completion test is a substring match.** It looks for `"COMPLETED"` in the console output, and that string
  also matches "…HAS NOT BEEN COMPLETED".
- **Unused results.** `exitCode` and `timedOut` are never checked.
- **Mitigation.** The `***ERROR` scan and the single-increment/time gate catch most failures.
- **Fix.** Require exit code 0, no timeout, and the exact completion phrase.

## What I verified and how

### Mathematics (`engine.cpp`) — matches SCIENCE.md §1–4 and Noether Thm 1–2 (`noether_derivation.md` §1.2, §4.1)

- **Signs.**
  - r = −RF (`abaqus_oracle.cpp:733`; synthetic `RF = f_int − f_ext`).
  - `applyTrue = −Δr/2ε` and the tangent check use K = −∂r/∂u.
  - The recursion is `K u^(k) = D^k r(pred)` with a plus sign.
- **Predictor.**
  - The order-k probes use `Σ_{l<k} m^l/l! u^(l)` in t-units, with `upTo = k−1` (`:314-322, 355`).
  - Commits use order K on every branch in lockstep (`:420-426`).
  - The m = 0 node is the measured replay residual r0, never assumed to be zero.
- **Stencils.** Fornberg weights are checked by the existing test. I checked by hand that the k-th weights applied to
  `m^k/k!` give 1. Offsets are −M..M, with M = 1 (K ≤ 2) and M = 2 (K = 3, 4), as documented.
- **Scaling.**
  - Pure derivatives use `1/h^k` (`:458`).
  - Mixed derivatives use `(D²(a+b) − D²(a−b))/(4 h_a h_b)`, which equals `4 h_a h_b u_ab/(4 h_a h_b)`. The order of
    the a+b/a−b pair (`d+1`) matches the construction (`:112`).
- **Fields.** Fields are differentiated from **commit** branches (`fd`, `f0`), never from probes (`:420-451`).
- **GMRES algebra.** As described under F5.

### Abaqus deck structure (read-only inspection of `scratchpad/abq_j2/branch_0/*.inp`)

- `*RESTART, READ, STEP=i−1, INC=1, END STEP` chains correctly, with one ABSS step per increment. Commit c2 reads
  step 1, and probe p4 reads step 3.
- `*BOUNDARY, OP=NEW` lists every DOF. Free DOFs are at the requested u, written with 17 significant digits.
  Constrained DOFs come from the **branch's own** (perturbed) deck evaluator.
- `*RESTART, WRITE, FREQUENCY=0` comes after the `*STATIC` data line. This is the placement Scout found necessary.
- Commits inherit the restart write from c1's model-level `*RESTART, WRITE, FREQUENCY=1`.
- The `KEYWORD IS IGNORED IN A RESTART` scan string matches Scout's observed warning.

### Test suite

- `ctest --test-dir build-core -LE abaqus`: 4/4 pass.
- `test_sensitivity -s`: 14 test cases and 539 assertions pass. The printed validation numbers match SCIENCE claims
  for the fixed-step synthetic cases.

### Coverage gaps

- No test checks a negative case for the validation metric, other than the Krylov-disabled control.
- No test checks detector recall.
- No test checks LoadEvaluator or the `.fil` readers.
- The Abaqus ctest checks only frame 1 at a single DOF.

## Reproduction

```powershell
$env:PATH="C:\Users\vfn333\toolchains\msys64\ucrt64\bin;$env:PATH"
ctest --test-dir build-core -LE abaqus --output-on-failure
# E0 (needs the builder's truss.abss), E1, E2, E5, E6:
.\validation\reference_models\vera\build_and_run.ps1 "<scratchpad>\abq_truss\truss.abss"
# add the detector sweep E3/E4 (~1 min):
& "$env:TEMP\vera_probe.exe" "<scratchpad>\abq_truss\truss.abss" sweep
```

## Acceptance conditions (minimum to lift the blockers)

1. **F1.** The E6 configuration, at default steps, is either Validated or flags every frame whose error vs reruns
   exceeds the tolerance. The threshold choice is justified by an asymmetry distribution over a smooth h-sweep.
2. **F2.** Validation returns Failed or Unverified for: non-finite values, missing (field, frame) entries, and zero
   compared values. E1 and E2 become regression tests.
3. **F3.**
   - The real-Abaqus A3 run is repeated with tightened `*CONTROLS` in the reruns, or with a reference-noise floor in
     the metric.
   - The A3 and A4 results are published with their commands in `docs/VALIDATION_RESULTS.md`, including any FAILs.
4. **F4 and F5.** Engine-side failures (GMRES non-convergence, re-probe non-convergence) are reported as such, not as
   physical non-smoothness.

F6–F10 are non-blocking improvements, but F6's FOLLOWER case should be refused before any NLGEOM follower-load model
is accepted.

## Remaining uncertainty

- I did not run Abaqus, so these points remain open:
  - **F3 cause:** the attribution to the 1e-8 linear tolerance is an inference from magnitudes.
  - **F6 semantics:** the load-replay behaviours are unverified.
  - **F7:** whether parallel jobs in one directory give identical results is untested.
- The F1/F4 evidence is from the synthetic oracle. How common off-path probe errors are on real Abaqus meshes is
  unknown.
- The in-progress j2 run (A4) had not finished when I reviewed. Its outcome is not part of this review.

## Stopping rule

This review ends here. The blockers have concrete regression tests. I will re-review only the fixes for F1–F5 against
the acceptance conditions above, not reopen the verified mathematics.

## Candidate lesson (proposed, not adopted)

A gate that compares the engine against full reruns at the **same** h and normalises by a global maximum can pass
vacuously: when values are NaN or missing, or when the comparison set is empty. Adaptive gates calibrated by hand can
also miss O(1) errors that the reviewed recommendation would have caught. Reviews should test the gate itself with
negative controls.
