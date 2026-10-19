# Validation results (measured)

- **Machine.** Intel Core Ultra 9 285K (24 cores), Windows 11 Enterprise, 2026-10-16.
- **Build.** MSYS2 UCRT64 GCC, Release, Kokkos 5.2.2 OpenMP (24 threads), SuiteSparse UMFPACK 6.3.8.
- **Abaqus.** Abaqus 2024 (build 2023_09_21-07.55.25 RELr426).
- **Provenance.** Every number below was produced by the listed command on this machine. Nothing is estimated or
  copied from the literature.
- **Error definition.** "Max rel. error" is the maximum over **all increments, all fields (U, RF, S, E, PEEQ,
  Mises) and all components** of `|engine − reference|`, divided by `max(|reference|, floor·max|F_nominal|/|a|^k)`.
  - The floor is 1e-4 for orders 1–2 and 1e-3 for orders ≥3 (SCIENCE §9).
  - The reference is the central FD of fully re-equilibrated reruns at `a ± m h` on the same time grid.

## 1. Build and test summary

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1      # fresh configure + build + ctest -LE abaqus
```
- Exit code 0; 62 s total wall time; 0 compiler warnings after cleanup.
- **6/6 ctest modules pass:**

| Module | Test cases | Assertions | Time |
|---|---|---|---|
| core | 7 | 219 | 0.9 s |
| inp | 36 | 17,932 | 1.0 s |
| linalg | 4 | 7,218 | 1.3 s |
| oracle | 4 | – | 1.8 s |
| sensitivity | 19 | 551 | 18 s |
| gui (offscreen) | 11 | 175 | 2.3 s |

`ctest -L abaqus` (real Abaqus, §3.1) passes separately in 129 s.

## 2. Synthetic models (in-house J2 FE oracle; exact consistent tangent unless stated)

Command: `build\bin\test_sensitivity.exe -s`. All cases are validated **at every increment**.

### 2.1 Analytic two-bar truss (`u2 = F L / (2 E A) + g/2`, 4 increments)
| Quantity | Result |
|---|---|
| du2/dE, d²u2/dE², d³u2/dE³ vs. analytic | rel. tolerances 1e-6, 1e-5, 1e-3 — pass |
| du2/dF, du2/dg (parameter-dependent prescribed BC), dU3/dg on the prescribed DOF | 1e-8, 1e-8, 1e-10 — pass |
| d RF1(fixed node)/dg = −(EA/L)·amp/2 | 1e-6 — pass |
| ∂²u2/∂E∂F (mixed, polarization) vs. analytic −L/(2E²A)·amp | 1e-6 — pass |
| First-order result at h/E = 1e-1, 1e-2, 1e-4 | exact to < 1e-9 (the residual is linear in E, so there is no truncation error) |

### 2.2 Elastic-plastic truss (6 increments, load then unload), full-rerun validation
| Parameter | Order 1 | Order 2 |
|---|---|---|
| E | 4.2e-6 | 1.5e-6 |
| σ_y | 2.2e-12 | 2.1e-5 |
| H | 2.1e-5 | 1.9e-5 |

Step convergence for H (order 1) against an h = 1e-5 run: h/H = 0.2 → 8.9e-2, 0.1 → 1.6e-4, 0.05 → 3.9e-5.

### 2.3 Plate with a hole (CPE4, 36 elements, J2, 10 increments: load / unload / reload), h/a = 1e-4
| Parameter | Order 1 | Order 2 |
|---|---|---|
| E | 6.2e-8 | 1.5e-6 |
| σ_y | 1.3e-7 | 2.3e-6 |
| g_top (prescribed displacement) | 8.5e-7 | 1.8e-6 |
| R_hole (**geometry**) | 4.9e-8 | 1.4e-6 |
| mixed ∂²/∂E∂σ_y (4-corner reruns) | – | 4.6e-7 |

**Step-size sweep** (E, σ_y and g_top; worst over the three; `diag: plate step sweep`):

| h/a | Order 1 | Order 2 |
|---|---|---|
| 1e-2 | 0.07–0.76 (fails: the stencil straddles yield onsets) | fails |
| 1e-3 | 0.03–0.19 (fails) | fails |
| 1e-4 | 6e-8 – 9e-7 | 2e-6 |
| 1e-5 | 6e-10 – 9e-9 (clean O(h²)) | 1.5e-4 (rerun noise ~tol/h²) |

### 2.4 3D cantilever (C3D8-like hexes, J2, 8 increments with unload / reload)
| Parameter | Order 1 | Order 2 | Note |
|---|---|---|---|
| E | 1.6e-7 | 2.9e-4 | 6×2×2 mesh |
| σ_y | 4.4e-8 | 4.2e-5 | |
| F_tip | 2.1e-8 | 6.5e-5 | |
| L_beam (**geometry**) | 9.6e-4 | 1.2e-3 | default steps. Before the review fix (Vera F1) this was **2.1 / 8.6**. |
| E, F_tip on the default 10×3×3 demo mesh | 5.5e-8, 8.6e-8 | – | GUI demo shows Validated |

### 2.5 Inconsistent tangent (oracle returns the elastic stiffness, as Abaqus does under plasticity)
- **Detection:** the tangent check triggers matrix-free GMRES at 3 truss increments and 7 plate increments.
- **With GMRES:** results are validated, for example plate E 6.2e-8 / 1.5e-6 and σ_y order 2 2.3e-6.
- **Negative control** (refinement disabled): σ_y order 1 on the truss is **Failed**, as expected.

### 2.6 Review regressions (Vera)
- An all-NaN series, a run cancelled before increment 1, and an empty field filter are all **not** Validated.
- With re-probing disabled on the cantilever L_beam case, 4 increments are wrong by more than 1e-3. All of them are
  covered: the onset is flagged and the later increments inherit from it.

## 3. Real Abaqus 2024

### 3.1 T2D2 truss vs. analytic formulas (ctest label `abaqus`)
Command: `ctest --test-dir build -L abaqus --output-on-failure` (129 s, 4 concurrent jobs). It drives the nominal job,
the replay branch, ±h restart chains and probes.

| Quantity | Engine (Abaqus oracle) | Analytic | Check |
|---|---|---|---|
| du/dE at the tip | −2.5e-7 | −FL/(E²A) = −2.5e-7 | rel. 1e-6 — pass |
| d²u/dE² | 2.5e-12 | 2FL/(E³A) = 2.5e-12 | rel. 1e-3 — pass |
| du/dF | 5e-5 | L/(EA) = 5e-5 | rel. 1e-9 — pass |
| Tangent check (`*ELEMENT MATRIX OUTPUT` K vs. oracle FD) | < 1e-8 (measured 3.1e-12) | | pass |

### 3.2 Truss with full-rerun validation, order 2 (A3): pre-review build (FAIL disclosed), then the current build (all pass)
Command: `abss-cli run examples\models\truss.inp --params "mat:STEEL:elastic:E,load:LOAD:cload:TIP:1,geom:TIP:X" --order 2 --validate`
(352 s engine, 488 s total, sequential jobs).

| Parameter | Order 1 | Order 2 |
|---|---|---|
| E | **FAIL 0.010** (field S) | 2.3e-4 |
| CF1 at tip | 7.1e-13 | 4.3e-4 |
| Move set TIP in X (geometry) | 1.2e-5 | 2.8e-4 |

- **The FAIL is disclosed.** dS/dE is exactly 0 for this statically determinate truss. The engine gave about 5e-12,
  which is about 1e-8 of the natural scale |S|/E. The old metric floor (1e-6) turned that into a relative error of
  0.01.
- **After the fix.** Since the review fix, the floor is 1e-4 and the Abaqus reruns use `*CONTROLS` R_n = 1e-8.
  **Repeated with the current build** (`--jobs 4`, 449 s total, engine 210 s). **All six are Validated:**

| Parameter | Order 1 | Order 2 |
|---|---|---|
| E | 1.0e-4 (S; the true value 0 is resolved to ~5e-12) | 2.3e-4 |
| CF1 at tip | 7.1e-13 | 4.3e-4 |
| Move set TIP in X (geometry) | 1.2e-7 | 2.8e-4 |

  Predictor consistency was 1e-8 to 1e-12, the lower-order leak at order 2 was at most 1.1e-10, and the tangent
  check was 3.1e-12.
- Load/BC replay self-check: 0 mismatch. Tangent check: 3.1e-12.

### 3.3 CPS4 J2 plasticity, 20 increments (load 10 / unload 10), order 1, every increment (A4)
Command: `abss-cli run examples\models\j2_plastic.inp --params "mat:STEEL:elastic:E,mat:STEEL:plastic:sigma_y:1,bc:LOAD:RIGHT:1-1" --order 1 --jobs 6 --validate`

- **Build.** The post-review build, apart from two later changes: `EXTRAPOLATION=NO` in reruns, and the zero-effect
  guard in the consistency flag described below.
- **Run.** 6 concurrent Abaqus jobs, 5,341 s wall time (engine 5,090 s). Restart chains ran in shared branch
  directories, with no job collisions observed (Vera F7: measured, passes in this run).

| Parameter | Status | Max rel. error vs. full Abaqus reruns (all 20 increments, 960 values, U/RF/S/E/PEEQ/Mises) |
|---|---|---|
| E | **Validated** | 5.0e-4 (worst field U) |
| σ_y (first point of the hardening table) | **Validated** | 3.2e-4 (U) |
| Prescribed U1 at RIGHT (step LOAD) | **Validated** | 1.6e-5 (E) |

- **Tangent.** At the 9 elastic increments (1–2 and 11–17) the Abaqus element-matrix K passed the tangent check at
  4e-11 to 1.3e-10. At the 11 plastic increments (3–10, 18–20) the check gave **0.15–0.32**: Abaqus supplied the
  elastic K, as Scout and Curie predicted. The engine switched to matrix-free GMRES, using 12 operator applications
  (24 probe jobs) per increment, and reached a worst relative residual of 1.8e-10.
- **One false flag (fixed in the current build).** The run flagged increments 1–2 for parameter E as "predictor
  inconsistent" (ratio 1e5). Under pure displacement control, E has no first-order load at an elastic increment, so
  the ratio divided noise by noise. The derivative itself validated. The current build skips the ratio when the
  first-order load is below 1e-8 of the force scale (regression test in `test_review_regressions.cpp`). The run has
  not been repeated with that change.

### 3.4 Independent Council experiments on Abaqus 2024 (Curie; scripts in `validation/experiments/curie/`)
These used Curie's own Python and Abaqus-Python tooling, not the ABSS C++ engine. They independently confirm the
mechanism ABSS implements.

| Experiment | Result |
|---|---|
| E1: r = −RF with all DOFs prescribed (truss, CPS4 patch; CLOAD on constrained DOFs, P2 pressure) | rel. 3e-16 / 1.8e-15 (double-precision output) |
| E2/E2d: history replay and restart chains (CPS4 J2, 20 increments, load / reverse yield / reload) | S to 1e-12, PEEQ to 1e-17 |
| E3: elastic du/dE from the pseudo-load | 2.7e-14 (h/E = 1e-2); predictor residual exactly O(h²) |
| E4: NLGEOM neo-Hookean with follower pressure, one-shot oracle | residual 2.3e-11. Hypoelastic `*ELASTIC` needs history replay: 83% error one-shot, 2.7e-9 with replay |
| **E5: end-to-end first-order plastic sensitivity**, 2×2 J2 plate, 10 plastic increments, 140 Abaqus jobs, tangent from oracle FD columns | **6.0e-5 to 1.1e-4 at every increment** against converged central FD of complete re-solves; the error falls linearly with the tangent FD step |

### 3.5 User subroutines: UMAT and UEL, source / object-only / library-only (`test_abaqus_usersub.cpp`)
Environment: Abaqus 2024 **without** its user-subroutine development files (`aba_param.inc` and the import
libraries are absent, so `abaqus make` cannot work), Intel ifx 2025.1.1, and MSVC 14.44 link. ABSS used its direct
build. Each subroutine was compiled once from `examples/subroutines/*.f`. Two further folders were then staged: one
holding only the `.obj`, and one holding only the built DLLs, with no source and no object. Each form was then run
through the full ABSS pipeline (nominal job, probes, commits, order 1, 4 concurrent jobs).

| Model | Parameter | Analytic | Forms | Result |
|---|---|---|---|---|
| UMAT, unit cube, uniaxial ε = 1e-3, 4 increments | dU2(node 3)/dPROPS2 (ν) | −1e-3 | source, obj, library | pass, rel. 1e-6 |
| | dU2/dPROPS1 (E) | 0 | all three | pass, \|·\| < 1e-12 |
| | dRF1(node 3)/dPROPS1 | 2.5e-4 | all three | pass, rel. 1e-5 |
| | tangent check (frame 0) | — | all three | < 1e-6 |
| | full dU/dν field: obj and library vs. source | — | | agree to 1e-10 |
| UEL truss (`*UEL PROPERTY`) | du(node 2)/dPROPS1 (E) | −2.5e-7 | source, obj, library | pass, rel. 1e-6 |
| | du/dPROPS2 (A) | −5e-3 | all three | pass, rel. 1e-6 |

Doctest reported 2 test cases, 112 assertions, 0 failures, after 1518 s (wall clock). The first attempt failed in the
UEL case before any Abaqus job ran. The cause was a second compiler-environment load in the same process (vcvars64
on an already-extended PATH). The fix is to load the environment once per process.

## 4. Performance (measured with `abss-cli bench --n 400`, 24 threads)
| Kernel | Serial | Kokkos OpenMP | Speedup |
|---|---|---|---|
| FD stencil over 5 × 3.2M doubles (output-derivative kernel) | 0.0111 s | 0.0042 s | 2.63× |
| SpMM, n = 160,000, nnz 0.8M, 16 RHS | 0.0065 s | 0.0022 s | 2.93× |
| Jacobi-PCG, n = 22,500, 312 iterations | 0.0261 s | 0.1150 s | **0.23× (slower)** |

| Direct solver, n = 22,500, 12 RHS | One factorization reused | Refactor per RHS | Gain | Residual |
|---|---|---|---|---|
| Eigen SparseLU | 0.070 s | 0.689 s | 9.9× | 4.8e-12 |
| Eigen SimplicialLDLT | 0.032 s | 0.242 s | 7.6× | 1.3e-11 |
| SuiteSparse UMFPACK | 0.080 s | 0.575 s | 7.2× | 1.1e-12 |

Whole synthetic engine run, plate with a hole (357 nodes, 320 elements, 10 increments), 7 parameters, order 2:
0.41 s, 464 oracle evaluations.

On real Abaqus the cost is dominated by job start-up (~13 s per probe or commit job; Scout, Curie). Compute
acceleration does not change that. No comparison with the old Plotly viewer was measured, because the two do
different work.
