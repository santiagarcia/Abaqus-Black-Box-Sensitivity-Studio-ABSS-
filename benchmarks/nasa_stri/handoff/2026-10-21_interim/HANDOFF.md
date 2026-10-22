# ABSS: NASA STRI campaign handoff (interim, evidence cutoff 2026-10-21 16:30 local)

This is from the ABSS repository's session, for the deck session (autoppt-01). Everything below marked COMPLETE is a
finished measurement. Anything marked RUNNING or NOT STARTED must not be shown as a result. A second handoff will
follow when the campaign ends. The Ti-6242 specifics are in a separate, git-ignored file:
`validation/private/benchmarks/handoff/HANDOFF_TI_2026-10-21.md`.

## 1. Versions and machine

| Item | Value |
|---|---|
| Baseline commit | `3ef872b8f9882897c16bb9a1190bc842032ba852` (branch `main`) |
| Campaign binaries | Frozen builds of the **uncommitted** working tree on top of 3ef872b: `v5` for family A, `v6` for families B and before/after, `v7` for Ti (v7 = v6 + SDV reader fix). They will be committed when the campaign ends, and the hash added to the final handoff. `benchmarks/nasa_stri/results/ENVIRONMENT.md` lists every binary's content. |
| Abaqus | 2024, build 2023_09_21-07.55.25 RELr426 |
| Licence | FlexNet, 5 tokens per job |
| Machine | Intel Core Ultra 9 285K (24 cores), 127.5 GB, NVMe, Windows 11 26200 |
| Per job | `cpus=1` (1 thread) for every Abaqus job of both methods |
| Concurrency | 8 simultaneous jobs for both methods in the matched-parallel stages (`_j8`); 1 in the serial stage (`_j1`) |

## 2. Status

| Analysis | Status | Config | Results |
|---|---|---|---|
| Profiling (instrumentation, job anatomy) | COMPLETE | `abss-cli --profile` | `results/pilot/`, `docs/PERFORMANCE.md` §1 |
| Optimizations (concurrent waves, fused commit+probe, ODB off, parallel validation, concurrent GMRES, coloured tangent, replicated branches) | COMPLETE, verified | — | `docs/PERFORMANCE.md` §2, `results/fusion_check`, `results/v4_check` |
| Mesh scaling, family A (elastic, P = 4, 8 slots, 2 repeats) | RUNNING. Repeat 1 complete except one failed run (see below); repeat 2 complete up to 48,672 DOFs. ETA about 17:00. | `plans/A_mesh_j8.json` | `results/A_mesh_j8/` |
| Parameter scaling (P = 1, 4, 16, 33 at 15,552 DOFs) | NOT STARTED. ETA 17:00–19:00. Single runs. | `plans/A_params_j8.json` | `results/A_params_j8/` |
| Family B (J2 load/unload, P = 4) | NOT STARTED. ETA about 19:00–21:00. | `plans/B_mesh_j8.json` | `results/B_mesh_j8/` |
| Before/after (v1 original schedule vs current) | NOT STARTED. ETA about 21:00–22:00. | `plans/before_after.json` | `results/before_after/` |
| Family C, Ti-6242 | NOT STARTED. ETA about 22:00–01:00. Private. | `validation/private/benchmarks/plans/C_ti.json` | `validation/private/benchmarks/results/C_ti/` |
| Increments study, serial reference | NOT STARTED. ETA after Ti. | `plans/A_inc_j8.json`, `plans/A_mesh_j1.json` | `results/...` |

All paths are relative to `benchmarks/nasa_stri/` unless rooted. Run ID convention:
`<case>__<method>_j<slots>_r<repeat>`, with `.json` for the profile and configuration, plus `.log` and `.abss`.

**Failed run.** `A96x12x12_inc4_P4__abss_j8_r1` failed in 7 s because of stale files from an interrupted earlier run
in the same scratch folder. It is excluded and will be re-run. The driver now clears scratch folders before each run.

## 3. Timing (COMPLETE runs, family A, P = 4, 4 increments, 8 slots)

Raw seconds from the interim table `summary_A_mesh_partial.csv` in this folder, repeat 1 / repeat 2:
- "nominal" is the method's own nominal analysis on the same machine and concurrency;
- "sens" is the sensitivity stage only.

| Free DOFs | ABSS replicated: nominal / sens | ABSS per-branch: sens | FAFD: nominal / sens |
|---|---|---|---|
| 432 | 16.7 / 72.2; 21.5 / 73.1 | 174.8; 161.2 | 31.5 / 15.0; 24.4 / 14.5 |
| 2,400 | 23.8 / 83.1; 24.4 / 81.9 | 163.4; 178.3 | 18.6 / 17.3; 26.8 / 15.7 |
| 7,056 | 23.1 / 100.6; 35.1 / 111.1 | 219.5; 229.6 | 28.6 / 17.8; 31.4 / 22.2 |
| 15,552 | 37.2 / 155.3; 19.1 / 144.5 | 256.3; 178.5 | 26.6 / 22.7; 28.9 / 20.9 |
| 48,672 | 63.4 / 472.3; 57.7 / 459.7 | (r1 failed); 431.0 | 45.7 / 48.0; 36.0 / 39.0 |
| 110,976 | 95.8 / 1755.8 (r1 only so far) | 1833.1 (r1) | 56.4 / 228.0; 61.6 / 158.8 |

**Crossover:** none measured so far, and the measured range does not suggest an approaching one. At matched 8 slots
and P = 4, FAFD's sensitivity stage is 5–10 times faster than the best ABSS variant at every mesh size. The ratio
T_FAFD / T_ABSS for the sensitivity stage, using replicated ABSS, is:

| Free DOFs | T_FAFD / T_ABSS |
|---|---|
| 432 | 0.20 |
| 2,400 | 0.20 |
| 7,056 | 0.18 |
| 15,552 | 0.15 |
| 48,672 | 0.09 |
| 110,976 | 0.13 |

Measured reason:
- Every Abaqus job costs about 17.5 s of driver start-up and licence checkout, the same for both methods.
- ABSS needs a few jobs **per increment**: 2P + 3 evaluations, packed into one job per slot when replicated. FAFD
  needs 2P jobs for the **whole** analysis.
- Each ABSS job also reads and writes the full restart state and writes full-field output. In the 111k-DOF case the
  `.mdl` alone is 143 MB per job. That grows faster with mesh size than FAFD's elastic solves do.

The parameter-count and nonlinear (family B) stages test the regimes where ABSS could gain. **Nothing about those
should be stated yet.**

## 4. FAFD vs ABSS

- **FAFD** is `abss-cli fafd`, the baseline and not ABSS. Its output is labelled FAFD.
  - It runs 2 complete Abaqus analyses per parameter at a ± h, central first-order differences.
  - Each analysis is forced onto the nominal analysis's accepted increments (fixed `DIRECT` increments,
    `EXTRAPOLATION=NO`), with the same tight equilibrium controls as ABSS's nominal (R_n = 1e-8) and the same output
    fields.
  - The same h, model, parameters and concurrency are used for both methods.
  - FAFD's "nominal" is one plain analysis on that grid.
- **ABSS** computes the same first derivatives from restart-chained history branches. See §7.
- **Step size.** Relative h = 1e-5, so h = 1e-5 · |a| (absolute 1e-5 for zero-valued parameters), for both methods.
- **Accuracy reference** in the campaign: FAFD. The comparison is `abss-cli compare`, per derivative and field over
  increments 1..n and every location:

  scaled error = max |ABSS − FAFD| / max |FAFD| over that field.

  It has no floor. Files: `results/<stage>/cmp_*.json` (computed at the end of each stage).
- **Validation metric**, which is different and used for "Validated" status: per field,

  max |ABSS − rerun-FD| / max( max |rerun-FD| over the field, 0.05 · max |nominal field| / |a|^k ).

  The pass threshold is 1e-3 at order 1. **The 0.05 floor is generous:** any absolute error below
  5e-5 · max|F| / |a| passes. `SCIENCE.md` used to say 1e-4 and has been corrected to 5e-2 today.

## 5–6. Ti-6242

See the private handoff, which covers the SDV erratum and the parameter counts.

## 7. Method details for slides

**(a) Residual probe.**
1. Restart from the branch's own committed state at the end of increment i−1. That is the branch's history at its
   own perturbed parameter values.
2. Run one increment with **every** independent DOF prescribed:
   - free DOFs at the probe state;
   - BC DOFs at the branch's own boundary values;
   - loads at the branch's own values.
3. The residual is r = −RF. The probe step writes no restart data, so its state is discarded and the history is
   untouched.

**(b) Commit.** A branch advances its history the same way, with its DOFs prescribed at the order-K predictor
u\* + Σ_l (m h)^l / l! · u^(l), so the branch history is correct to O(h^(K+1)). The commit job writes restart data,
which is the history for increment i+1, plus the full fields that are differentiated. Since the optimizations, the
same job also runs the next increment's first probe as a second step.

**(c) Tangent check and corrections.**
- **Check:** compare K_oracle · v (element matrices from the nominal job) with the central difference of 2 probes at
  u\* ± ε v, with ε = 1e-6 · max|u\*_F|.
- **Rule:** if the relative mismatch exceeds 1e-5, the oracle tangent is treated as inconsistent. ABSS then uses the
  cheaper of two options, by estimated Abaqus job rounds:
  - **assembled true tangent:** column-grouped probes, 2 per group; used only when the element pattern is complete
    (no constraints or contact), and accepted only if it reproduces the check probes row by row to 1e-4;
  - **matrix-free GMRES:** each product is 2 probes; preconditioned by the oracle tangent; tolerance 1e-8.
- The check result and the path taken are logged per increment.

**(d) Why every perturbed branch starts at t = 0.** A branch is a separate analysis of an *edited* deck: material
constants, loads, BCs or geometry at a + m h. Abaqus restart cannot change model data, and ABSS also treats the
"KEYWORD IS IGNORED IN A RESTART" message as an error. A perturbed history must also start at the perturbed initial
state to be the history of a + m h. So each branch is its own restart chain from t = 0.

## 8. Known failures and outdated documents

- **Outdated:** `docs/KNOWN_LIMITATIONS.md`, the "Cost" and "Speed" bullets. They give "13 s start-up" (now measured
  at 17.5 s) and old job counts. They will be rewritten from the final campaign.
- **Superseded, kept as originally recorded:** `docs/VALIDATION_RESULTS.md` §3.8, the old timing table (1.3–61× slower
  on 12 small models, 3 slots). It stays as part of the original verification record.
- **Fixed today:**
  - the `SCIENCE.md` validation floor;
  - a Ti SDV erratum added to `VALIDATION_RESULTS.md` §3.7b.
- **Run failure:** the stale-scratch failure (§2).

## 9. GUI

The committed app at 3ef872b, the demo-video version, is still representative. Since then, the only UI changes are
uncommitted and not yet built into the GUI:
- a checkbox on Set up ▸ Abaqus, "Run several history branches in each Abaqus job";
- two cost-estimate lines (Abaqus jobs per increment).

## 10. Stable location

This folder is the frozen interim snapshot: `benchmarks/nasa_stri/handoff/2026-10-21_interim/`. The final report will
be `benchmarks/nasa_stri/REPORT.md` and a second handoff folder, announced by message.
