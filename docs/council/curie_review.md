# Curie review: residual/reaction oracle and history strategy

- Reviewer: Curie (computational mechanics; builder-experimenter for these experiments, independent reviewer #1 of the spec's oracle and history design)
- Date: 2026-10-16
- Independence: written without reading other Council members' outputs in `docs/council/`. I built and ran the experiments below myself, so they need a second, independent check (Vera or another member) before anyone treats them as verified.
- Affect layer: enabled, low influence. It only changed what I explored: the NLGEOM-hypoelastic and `*MATRIX GENERATE` checks were added out of curiosity about "elastic is history free" and "the matrix is the tangent".
- Environment: Abaqus 2024 (build 2023_09_21-07.55.25 RELr426 190762), Windows 11, 1 CPU per job, jobs run one after another. Abaqus Python is 3.10.5 with numpy. No C++ or Fortran compiler is visible to Abaqus (`abaqus info=system`: "Unable to find a Fortran compiler"; C++ "Unable to locate"), so I tested no UMAT or UEL.
- Code and results: `validation/experiments/curie/` (scripts), `validation/experiments/curie/results/*.json|*.log.txt` (measured numbers). Generated INPs are in `validation/experiments/curie/work/<exp>/`. ODB, SIM, RES and the other heavy files are gitignored.

## 1. Conventions measured (not assumed)

The sign convention is **RF = F_int(u) − F_ext**, at every prescribed DOF. With `r = F_ext − F_int`, this gives **r = −RF exactly** and **K = −∂r/∂u = ∂RF/∂u**.

- **RF includes all applied loads acting on a prescribed DOF.** A `*CLOAD` on a constrained DOF is subtracted inside RF. Two alternative hypotheses are falsified by a margin of the size of the load itself:
  - "RF = K·u" is off by 2000 (truss) and 500 (patch).
  - "RF excludes CLOADs on constrained DOFs" is off by 500 (truss) and 30 (patch).
- **Distributed pressure (`*DLOAD P2`) is in RF** as its consistent nodal load. For plane stress the nodal load includes the section thickness. The alternative "not multiplied by thickness" is off by 500.
- **CF output** gives only the concentrated part of F_ext, at all DOFs (constrained ones included). It does not include DLOAD.
- **Precision.** The default ODB stores single-precision U and RF. In E1 that gave a relative oracle error of 1.4e-8. With the command-line option `output_precision=full`, the same values come out in double precision (`fieldValue.precision == DOUBLE_PRECISION`, read from `dataDouble`) and the errors drop to 1e-15. **ODB frame times (`frameValue`) stay float32 either way** (e.g. 0.30000001192092896).
- **A job with every DOF prescribed (0 equations) runs fine.** Abaqus solves it and reports RF at all DOFs. No dummy free DOF is needed.

## 2. Experiments and numbers

All residual comparisons below use double-precision output unless stated otherwise.

### E1: frozen-displacement reaction oracle on linear models (`e1_oracle.py`)

Setup:
- Models:
  - (a) a 3-bar T2D2 truss with a nonzero prescribed U1 = 0.05, and CLOADs on a free DOF, a zero-constrained DOF and the nonzero-prescribed DOF;
  - (b) a 2×2 distorted CPS4 patch, thickness 0.5, with CLOADs (one on a constrained DOF), P2 pressure on the right edge and a nonzero prescribed U2 = 0.01.
- Procedure: solve the nominal model, then set ũ = u* + δ on **all** DOFs. δ is about 1e-2 of |u| for the truss and 1e-3 absolute for the patch. The oracle INP prescribes every DOF to ũ and keeps all the original loads.
- Reference: an analytic K computed in numpy (validation only). For CPS4 I cross-checked it against `*MATRIX GENERATE` with `FORMAT=MATRIX INPUT`: max relative difference 3.6e-15. So plane-stress CPS4 is plain 2×2 full integration.

| quantity | truss | CPS4 patch |
|---|---|---|
| max abs(RF − (K ũ − F_ext)), double | 4.5e-13 (rel 3.1e-16) | 9.4e-13 (rel 1.8e-15) |
| same, single-precision ODB | 2.2e-5 (rel 1.5e-8) | 7.3e-6 (rel 1.4e-8) |
| max abs(RF − K ũ) (hypothesis "loads not in RF") | 2000 | 500 |
| max abs(RF − (K ũ − F_ext without CLOADs on constrained DOFs)) | 500 | 30 |
| nominal: free-DOF imbalance of the analytic K at u* | 1.1e-13 | 6.8e-13 |
| prescribed ũ read back from the ODB | 1.7e-18 | 1.7e-18 |

### E2: history reproduction for J2 plasticity (`e2_history.py`, `e2d_restart_chain.py`)

Setup:
- Model: 3×3 distorted CPS4 plate with J2 isotropic hardening (σ_y = 250, piecewise-linear hardening).
- Loading: the top edge is displacement-controlled with amplitude 0 → 1 → 0.3 → 1.2 (load, then unload with reverse yielding, then reload). There are CLOADs on a free DOF and on a prescribed DOF.
- Increments: `*STATIC, DIRECT` with 20 × 0.05, `EXTRAPOLATION=NO` (the default extrapolation diverged at the unloading kink), equilibrium tolerance R_n = 1e-8.
- Mechanisms compared:
  - (A) **replay-amp**: one step; every DOF gets its own TABULAR amplitude through the accepted history.
  - (B) **replay-step**: one `*STATIC, DIRECT` step per accepted increment; every DOF is prescribed to that increment's value.
  - (C) **restart**: `*RESTART, READ, STEP=1, INC=i-1, END STEP`, then a new one-increment step that prescribes every DOF.
  - (C′) **restart chain**: restart of a restart of a restart, each link committing one increment. This is the commit-branch pattern.

| check | result |
|---|---|
| (A) S over all 20 increments, against the free run | max abs dS 3.3e-12 (rel 1.0e-14); max abs dPEEQ 3.5e-17 |
| (B) same | max abs dS 1.6e-12 (rel 5.0e-15); max abs dPEEQ 2.8e-17 |
| (A), (B) RF on originally free DOFs | at most 3.5e-7, which is the free run's own Newton residual (R_n = 1e-8) |
| (A), (B) RF on originally constrained DOFs, against the free run | at most 9.2e-12 |
| probe at i = 6, 11, 13, 18, δ = 0: replay against restart | RF diff ≤ 4.4e-12, S diff ≤ 1.5e-12, PEEQ diff ≤ 3.5e-17 |
| probe with δ (abs(δ) ≤ 1e-4, random): replay against restart | RF diff ≤ 4.3e-12 |
| (C′) chain of 3 restart links (increments 6, 7, 8) against the free run | dS ≤ 2.6e-13, dPEEQ ≤ 5.2e-18 |
| elastic increment 13 (max ΔPEEQ = 0): ΔRF(δ) against K_el δ | rel 5.4e-14 (replay and restart) |
| plastic increments 6, 11, 18: ΔRF(δ) against K_el δ | rel 0.59 to 0.66 (as expected: the consistent tangent is not elastic) |
| plastic increments: linearity of the probe, δ against δ/10 | rel diff 3.3e-3 to 4.5e-3. In elastic increment 13 it is 4.2e-13 |

Practical notes:
- **Restart needs `.res .mdl .stt .prt .odb` and also `.sim`.** Without `oldjob.sim`, pre.exe crashed with "THE API HAS DETECTED AN INTERNAL LOGIC ERROR ... CopyUnitSystem" (exit 300).
- One restart link for the 9-element model is about 3.2 MB on disk, of which the `.sim` is 1.6 MB.

### E2c: automatic incrementation and time grid (`e2c_autoinc.py`)

- **Grid change under perturbation:** not observed. With E perturbed by 1e-3, 1e-2 and 1e-1, the automatic accepted grid was identical: 15 increments of 0.07 with initial/max 0.07/0.13, and 4 increments of 0.25 with 0.25/0.5. This is **not a discriminating test**, because this model never cut back. The risk is not falsified.
- **Replaying the automatic (irregular) grid** with one DIRECT step per accepted increment reproduces S to 2.0e-12 and PEEQ to 1.7e-17 at all 15 increments.
- **Forcing a perturbed free run onto the nominal grid** (one DIRECT step per nominal increment) gives a time-grid difference of exactly 0.

### E2e: is `*MATRIX GENERATE` the tangent at a plastic state? (`e2e_matrix_tangent.py`)

Setup: a step-per-increment replay, with a `*MATRIX GENERATE, STIFFNESS` perturbation step inserted after increments 6, 13 and 18.

- The base-state BCs put **1e36 on the diagonal** of every constrained DOF. The original diagonal is lost. `*BOUNDARY, OP=NEW` inside the perturbation step removes this.
- **The generated matrix is the elastic K at every increment:** it matches the analytic K_el to 4.2e-15, including at the plastic increments 6 and 18.
- At those plastic increments it is off from the probe's directional derivative by 113% to 127% (relative to abs(ΔRF)).
- So `*MATRIX GENERATE` **is not the elastoplastic tangent**. I did not test `*OPERATOR OUTPUT`.

### E3: oracle sensitivity and predictor consistency, linear elastic (`e3_predictor.py`)

Setup: the E1 CPS4 patch with parameters a = E and b = the prescribed U2 at node 3.

- **K from black-box oracle FD columns** (18 oracle jobs, ε = 1e-3) matches the analytic K to rel 3.6e-15.
- **s_E from the pseudo-load** p = [r(u*, E+h) − r(u*, E)]/h, solved with K_FF s_F = p_F:

| h/E | rel err against analytic du/dE |
|---|---|
| 1e-2 | 2.7e-14 |
| 1e-4 | 1.9e-12 |
| 1e-6 | 2.0e-10 |

  The growth at small h is roundoff, and only possible because the output is in double precision. r is linear in E, so there is no truncation error.
- **Central FD of complete re-solves** (h/E = 1e-2, 1e-3, 1e-4) against the oracle s: rel diff 1.0e-4, 1.0e-6, 1.0e-8. This is the O(h²) truncation of the CFD reference itself (u_F ∝ 1/E). The oracle result sits on the analytic value.
- **BC parameter b:** at every-DOF-at-u* the oracle does not see b, so p = 0 and s_F = −K_FF⁻¹ K_FC s_C with s_C = e_b. Error against analytic: 7.1e-15; against CFD: 8.5e-15. Setting s_C = 0 would be 100% wrong.
- **Predictor residual:** max abs of r_F(u* + h s, E+h) for h/E = 0.1, 0.03, 0.01, 0.003, 0.001 is 5.0, 0.45, 0.050, 0.0045, 5.0e-4.
  - Observed order: 2.00.
  - It equals the analytic −h²·K1_FF s_F, with coefficient 1.25e-8.
  - The zeroth-order residual r_F(u*, E+h) is 50, 15, 5, 1.5, 0.5. Observed order: 1.00.

### E4: NLGEOM, follower pressure, and "elastic" history (`e4_nlgeom.py`)

Setup:
- Model: a CPS4 strip of 8×2 elements (40×4), clamped, with follower pressure on the top face.
- Nominal solve: NLGEOM, 10 DIRECT increments, R_n = 1e-8. Tip displacement is (−1.27, −7.34), i.e. a rotation of order 10° or more.
- Oracles:
  - **one-shot**: every DOF prescribed to u* in a single increment from the undeformed state;
  - **replay**: one step per accepted increment.

| material | one-shot max abs RF_free (relative to the pressure resultant 1.6) | one-shot max abs dS (relative to max abs S) | replay max abs RF_free | replay max abs dS |
|---|---|---|---|---|
| `*HYPERELASTIC, NEO HOOKE` | 2.3e-11 (1.4e-11) | 1.9e-13 | 2.3e-11 | 1.2e-13 |
| `*ELASTIC` (with NLGEOM, Abaqus integrates a hypoelastic rate form) | **1.33 (0.83)** | **0.235 (4.2%)** | 2.7e-9 | 1.3e-13 |

What this shows:
- The follower pressure is evaluated on the current (prescribed) configuration. Otherwise the neo-Hookean one-shot residual could not be 1e-11.
- **`*ELASTIC` under NLGEOM is path dependent.** A one-shot oracle gives a residual of 83% of the load. Such a model needs the same history replay or restart as plasticity.

### E5: end-to-end first-order history sensitivity of J2 plasticity (`e5_plastic_gate.py`)

Setup:
- Model: 2×2 distorted CPS4 plate, J2 (σ_y = 250), top-edge displacement control with amplitude 0 → 1 → 0.3 → 1.2, CLOADs on a free and on a prescribed DOF.
- Increments: 10 DIRECT increments of 0.1, R_n = 1e-10. **Plastic flow occurs in every increment**, including reverse yielding during unloading: max PEEQ is 2.8e-3, 7.1e-3, … , 3.5e-2.
- Parameter: E. There are 11 free DOFs.

Algorithm (spec §3), with every residual taken from the frozen-displacement oracle (r = −RF) and every history reproduced by full replay:
- r_nom = r(u*_i; nominal history, E).
- K_FF from 11 oracle FD columns with step ε, holding the nominal history i−1.
- r_pert = r(u*_i; committed perturbed history, E+h), with h = 1e-5·E.
- p = (r_pert − r_nom)/h, then solve K_FF s_F = p_F.
- Commit u*_i + h s_i as the next history row.

In total that is 140 Abaqus jobs.

Reference: central FD of complete free re-solves at E(1 ± h_ref) on the same DIRECT grid.

| quantity | result |
|---|---|
| rel err max abs(s − s_CFD)/max abs(s_CFD) per increment 1..10 (ε = 1e-6, h_ref = 1e-4) | 6.0e-5, 6.0e-5, 7.6e-5, 9.6e-5, 1.08e-4, 8.6e-5, 8.9e-5, 1.8e-5, 3.2e-5, 4.9e-5 |
| CFD reference converged in h_ref? | yes: h_ref = 1e-3, 1e-4 and 1e-5 give the same errors to 2 or 3 digits |
| ε-study, increments 1 and 2 (`--nprobe 2`) | ε = 1e-5: 5.2e-4, 5.4e-4; ε = 1e-6: 6.0e-5, 6.0e-5; ε = 1e-7: 1.4e-5, 1.2e-5 |
| asymmetry of the FD tangent, max abs(K − Kᵀ)/max abs(K) | 7.4e-5, 7.4e-6, 7.4e-7 for ε = 1e-5, 1e-6, 1e-7 (O(ε); the consistent J2 tangent is symmetric) |
| predictor residual at the commit, abs(r_F(u*+h s; committed hist, E+h)), compared with the zeroth-order abs(r_F(u*; committed hist, E+h)) | 1e-8 to 7e-7, compared with 7e-4 to 2.4e-3 (a single h only, so no order claim) |
| nominal oracle residual abs(r_F(u*_i)) | 4e-11 to 6e-7 (Newton tolerance) |

Interpretation:
- The oracle method reproduces the converged FD sensitivity at **every** plastic increment (load, reverse-yield unload, reload) to about 1e-4.
- The error falls linearly with the tangent FD step ε, down to a floor of about 1e-5. That floor is consistent with the O(h) truncation of the forward pseudo-load at h/E = 1e-5.
- This is first order only, on one tiny model. It shows that the mechanism works. It does not show that it is efficient: 11 oracle jobs per increment were needed just for K.

## 3. Recommended oracle construction

1. **Prescribe every nodal DOF that carries an equation**, the originally constrained DOFs included.
   - Free DOFs get ũ_F.
   - Originally prescribed DOFs get their BC value at (t_i, a), or the perturbed value for a parameter-dependent BC.
   - Then r = −RF on all DOFs. r_F is used in the solve. r_C gives the reaction sensitivities for free (dRF_C/da = −dr_C/da along the solution).
   - Do not leave originally constrained DOFs to their old `*BOUNDARY`. Write one complete `*BOUNDARY` table (or OP=NEW) so the oracle has no dependence on step-to-step BC inheritance.
2. **Keep all loads exactly as in the nominal increment:** CLOAD, DLOAD/DSLOAD, gravity and body loads, at the amplitude value at t_i. Write them as fixed magnitudes, or keep the amplitude with the identical time grid. CLOADs on prescribed DOFs need no correction, because RF already subtracts them (measured). Do not add CF back.
3. **Always evaluate the nominal r(u*_i, nominal history) with the oracle.** Do not assume it is zero. The converged Newton residual (3.5e-7 here, and about 0.5% of force at the default tolerance) cancels in p_i only if both terms come from the same oracle. Tighten the nominal tolerance (`*CONTROLS, PARAMETERS=FIELD`) for sensitivity runs.
4. **Always run with `output_precision=full`** and read `dataDouble`. Single precision limits FD pseudo-loads to about 1e-8/h relative noise. That makes second order unreachable.
5. **Use the time grid from the input (DIRECT) or from the .sta file**, never from ODB `frameValue` (float32).
6. **MPC, tie, coupling, rigid bodies** (untested, so treat this as a hypothesis to check):
   - Prescribe only the **independent** (retained) DOFs.
   - Never prescribe an MPC-dependent or tie-slave DOF. Abaqus eliminates it, and prescribing it conflicts with the elimination.
   - ũ must satisfy the constraint (ũ_dep = T ũ_ind), and RF on the independent DOFs is then the reduced residual Tᵀr.
   - Sensitivities live in reduced coordinates. Recover dependent DOFs through T, or through dT/da if the constraint depends on the parameter (geometry).
   - Block these models until test E7 (section 5) passes.
7. **Shell rotations** (untested): prescribe UR1 to UR3 as well. Under NLGEOM, finite-rotation BC semantics (Abaqus treats rotation-vector BCs specially in large rotation) must be validated before any S4/S4R NLGEOM support. Without NLGEOM, rotations are ordinary DOFs.
8. **NLGEOM follower loads** are handled correctly by the oracle (measured, E4). Under NLGEOM, **every material that is not hyperelastic is history dependent**, including `*ELASTIC`. Route all NLGEOM models through the history mechanism; one-shot evaluation is allowed only for documented hyperelastic models.
9. **Elements with extra solution variables** (hybrid pressure, incompatible modes, UEL internal or Lagrange DOFs, hard-contact Lagrange multipliers) are untested. When all nodal DOFs are prescribed, Abaqus still solves for those variables. The oracle then plausibly returns the statically condensed residual, which goes with a condensed K. This is plausible but unverified. Gate it on test E8 (section 5).

## 4. Recommended history mechanism

Both the replay and restart mechanisms reproduced the plastic internal state to round-off: S matched to 1e-12 (rel 1e-14), PEEQ to 1e-17, and the probe RFs to 4e-12. That holds for:
- single restarts from any increment;
- a three-link restart chain;
- per-DOF-amplitude replay;
- step-per-increment replay, including an irregular automatic grid.

The choice between them is therefore about **cost and robustness**, not about correctness:

- **Primary option, a restart chain for the committed perturbed branch:**
  - Link i restarts link i−1 (`STEP=i-1, INC=1`) and prescribes u*_i + h s_i with *RESTART WRITE*.
  - Probe i restarts the same link i−1 and prescribes u*_i.
  - That is 2 jobs per increment per stencil point, with O(1) increments each.
  - The branch's perturbed material is defined in link 0, the first full job, never mid-chain.
  - Requirements: keep `.res .mdl .stt .prt .odb .sim` per link (about 3 MB per link for 9 elements, far more for real models). Delete each link once i+1 is committed.
- **Fallback and verifier, full replay from t = 0:**
  - Use step-per-increment replay rather than per-DOF amplitudes. It has the same INP size, handles irregular grids, and avoids N_dof amplitude objects.
  - It needs no restart files, and each job is independent and restartable after a crash.
  - Its cost is O(i) increments per probe, so O(N²) per stencil point. Fully prescribed increments need one equilibrium pass, and on these models job start-up (**about 12 to 16 s per job measured, solver time under 1 s**) dominates both mechanisms.
  - The INP grows as N_dof × i lines, so this becomes the bottleneck for models with 10⁵ DOFs.
  - E5 used replay throughout.
- **Perturbed branches must run on the nominal accepted grid.** Use DIRECT steps per nominal increment. Never let automatic incrementation choose the perturbed grid, and do the same for the FD reference re-solves.

## 5. Risks, ranked

1. **Tangent K at nonlinear states (high).** `*MATRIX GENERATE` returns the *elastic* matrix at plastic states (E2e), so it is unusable for plasticity. Oracle FD columns cost N_free jobs per increment, which is prohibitive. `*OPERATOR OUTPUT` is untested, and it is the decisive next experiment. A fallback (speculative) is a Krylov solve (GMRES) using oracle directional derivatives as matvecs (1 job each), preconditioned with the elastic K from `*MATRIX GENERATE`.
2. **Job count and start-up overhead (high, UX and feasibility).** About 12 to 16 s per Abaqus job even with 9 elements. First order needs at least 2 jobs per increment per parameter per stencil point. On top of that, K needs either verified operator output or many matvecs. 5 Abaqus/Standard tokens per job (observed in the log) limit parallel branches.
3. **Constraints: MPC, tie, coupling, rigid body, connector (high).** Not tested. Prescribing dependent DOFs is ill-posed. The reduced-coordinate formulation is needed.
4. **Elements with internal or Lagrange variables, contact Lagrange multipliers, UEL with internal DOFs (high).** The oracle semantics there are unverified.
5. **Nonsmooth plasticity (medium-high).** The probe map at plastic increments is measurably nonlinear: a 3e-3 relative change between δ = 1e-4 and 1e-5. Yield activation can flip within the FD step. ε and h need two-step checks per increment, with a flag when they disagree.
6. **Shell finite rotations (medium).** Untested.
7. **Precision (medium, mitigated).** The default single-precision ODB gives 1.4e-8 relative noise. `output_precision=full` is mandatory, and frame times stay float32.
8. **Hypoelastic NLGEOM path dependence (medium, mitigated).** It must be routed through the history mechanism (E4).
9. **Automatic-incrementation drift (medium).** Not observed in E2c, but that test was not discriminating. Mitigation: always force the nominal grid.
10. **Restart file set and disk use (medium).** `.sim` is required; there are per-link file sets, and Abaqus version coupling: restart requires the same release.
11. **Dynamics, inertia, initial stress, predefined fields (out of scope).** The oracle must carry `*INITIAL CONDITIONS`, temperatures and field variables unchanged. Dynamics needs the effective tangent and prescribed velocities and accelerations. Do not claim support.

## 6. Decisive experiments still open

- **E6: `*OPERATOR OUTPUT, OPERATOR=ASSEMBLED, STIFFNESS`** inside the nonlinear J2 step. Compare against probe FD columns at plastic increments, using two ε values, and check row/column DOF labels via `mtxasm` text output. This decides risk 1.
- **E7: MPC/tie/coupling oracle.** Run a tie or `*EQUATION` patch in two variants: prescribe the independent DOFs only, and prescribe all DOFs (expected to be rejected). Compare against a reduced analytic T and K.
- **E8: hybrid (CPE4H/C3D8H) and incompatible-mode (CPS4I) oracles.** Check r(ũ) against the condensed analytic residual, and E5-style sensitivities against CFD.
- **E9: S4/S4R with rotations, linear and NLGEOM**, including rotation BC semantics.
- **E10: a case where automatic incrementation cuts back.** Show grid drift with a free perturbed run, and check that the forced-grid branch removes it.
- **E11: second order.** Use a polynomial-in-h recursion with the predictor u* + h s + h²/2 u'' on the E3 patch, where d²u/dE² is analytic. Then repeat on E5 with nonsmoothness flags.
- **E12: a compiled UMAT/UEL.** Blocked here because no compiler is available.

## 7. Dissent and refinements to the spec

- **Spec §2 ("r = −RF nominally, hypothesis"):** confirmed to machine precision for T2D2 and CPS4, with CLOADs on prescribed DOFs and with pressure (and with NLGEOM follower pressure). No load-correction term is needed. The spec should state that loads on prescribed DOFs are already inside RF.
- **Spec §3.4:** agreed that s_C ≠ 0 for parameter-dependent BCs (E3: setting s_C = 0 would be 100% wrong). Refinement: prescribing originally constrained DOFs in the oracle costs nothing and also gives r_C. Note also that for a BC parameter the frozen oracle gives p = 0. The whole sensitivity then comes from −K_FC s_C.
- **Spec §2/§7 on `*OPERATOR OUTPUT` and matrix generation:** I dissent from treating "Abaqus matrix output" as a likely tangent source. `*MATRIX GENERATE` is demonstrably elastic at plastic states. Operator output must pass E6 before the generic engine relies on it.
- **Spec §3.5 (restart vs replay):** both preserve q exactly (to 1e-12). I recommend the restart chain as the production path and step-per-increment replay as the verifier and fallback, not "replay only if restart fails".
- **Spec §3 history scope:** "history" must include **geometric path dependence**. Under NLGEOM, `*ELASTIC` is history dependent (one-shot residual 83% of the load).
- **Spec §7 gate "Operator matrix ≈ frozen-history residual directional derivative":** agreed. E2 shows the oracle directional derivative is exact at elastic increments and nonlinear at the 3e-3 level for δ = 1e-4 at plastic increments. The gate needs ε-convergence, not a single ε.
- **Missing from the spec:** `output_precision=full`, float32 frame times, `EXTRAPOLATION=NO` (or equivalent) for prescribed-history and unloading robustness, and the restart `.sim` requirement.

## 8. Reproduction commands

Run from `validation/experiments/curie/` in PowerShell. Each script writes `results/<exp>.json` and work files in `work/<exp>/`.

```
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e1_oracle.py --precision full"
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e1_oracle.py --precision single"
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e2_history.py"          # ~5 min, 31 jobs
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e2d_restart_chain.py"   # needs work/e2 from e2_history.py
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e2e_matrix_tangent.py"  # needs results/e2_history.json + work/e2
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e2c_autoinc.py"
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e3_predictor.py"        # ~9 min, 41 jobs
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e4_nlgeom.py"
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e5_plastic_gate.py"     # ~40 min, 140 oracle jobs + 7 free runs
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e5_plastic_gate.py --eps-k 1e-7 --nprobe 2 --tag epsK1e-7"   # needs the full run's JSON
cmd /c "C:\SIMULIA\Commands\abaqus.bat python e5_plastic_gate.py --eps-k 1e-5 --nprobe 2 --tag epsK1e-5"
```

Representative generated decks (nominal, all-DOF oracle, replay probe, restart probe, restart-chain link, mid-history matrix generation, NLGEOM one-shot) are copied to `validation/experiments/curie/examples/`.

`validation/experiments/curie/work/` holds about 300 regenerable INPs plus gitignored solver files. I recommend the repo owner add `validation/experiments/curie/work/` to `.gitignore`. I did not edit `.gitignore` because it is not my artifact.

Reusable extractor (for the C++ app to call as a subprocess):

```
abaqus python odb_dump.py JOB.odb OUT.json --fields U,RF,CF,S,PEEQ [--mesh] [--last-frame-only]
```

It writes nodal and integration-point values per step and frame with element, integration-point and section-point identities. It uses `dataDouble` when the ODB is double precision, records the precision, and does no averaging.

## 9. Limitations of this review

- Only small 2D continuum elements (T2D2, CPS4) and J2 or neo-Hookean materials were tested. No shells, 3D, constraints, contact, UMAT/UEL or dynamics.
- The analytic K is my own numpy reference. It matches `*MATRIX GENERATE` to 4e-15, but it was written by the same person who designed the tests.
- The E2c schedule-drift test was not discriminating.
- These experiments were built and run by Curie. They need an independent re-run or audit before they are recorded as verified findings.
