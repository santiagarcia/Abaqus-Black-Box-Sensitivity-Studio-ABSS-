# ABSS NASA STRI campaign: final handoff (evidence cutoff 2026-10-22 03:22 local, all stages complete)

This supersedes `../2026-10-21_interim/`. Everything here is COMPLETE and measured. The files in this folder are
frozen copies. The Ti-6242 details stay in the git-ignored
`validation/private/benchmarks/handoff/HANDOFF_TI_2026-10-21.md` and `summary_C_ti_final.csv`.

## Version

- **Code:** commit `375cb1a796f9d68b1c6b32f958e90a2ac8f4eab3` (`main`, pushed).
- **Measurement binaries:** frozen builds v5, v6 and v7 of the working tree that this commit contains
  (`ENVIRONMENT_frozen.md` lists which binary ran which stage). Commit 375cb1a = v7, plus GUI, docs and scripts.
  The GUI has one new option ("Run several history branches in each Abaqus job") and two cost-estimate lines;
  otherwise it is as in the 3ef872b demo video.
- **Abaqus:** 2024 RELr426.
- **Machine:** Intel Core Ultra 9 285K (24 cores), 127.5 GB.
- **Jobs:** `cpus=1` per job; 8 or 1 concurrent jobs, the same for both methods.

## Files

| File | Content |
|---|---|
| `summary_all_runs.csv` | 142 runs, all public stages: A_mesh_j8, A_params_j8, A_inc_j8, B_mesh_j8, before_after, serial_crossover, h_study, pilot. Columns include free_dofs, n_params, increments, jobs (slots), t_nominal, t_sensitivity, t_workflow, abaqus_jobs(_sensitivity), per-phase job seconds, GMRES/assembled-tangent counts, acc_worst_err_natural, acc_worst_err_scaled_floor and overlaps_build_or_check. |
| `per_job_breakdown_A48.csv` | Per-job phase table for one representative case (A48, P = 4, 8 slots, repeat 2): replicated, per-branch, FAFD. |
| `job_anatomy.txt` | Phase averages over the 6,065 campaign jobs. |
| `REPORT_frozen.md` | The full report. |
| `SLIDES_frozen.md` | Suggested replacement slides. |
| `PERFORMANCE_frozen.md` | Profiling, the changes and how each was verified. |
| `ENVIRONMENT_frozen.md` | Environment and version record. |
| `figures/` | All figures: PDF, SVG, PNG, large fonts. |

## Corrections to earlier messages

1. **Per-job overhead.** Earlier messages said "17.5 s driver + licence per job". That was one manual timing of a
   single job. The average over 6,065 campaign jobs is:

   | Phase | Seconds per job |
   |---|---|
   | Driver + licence | 6.3 |
   | Input processing | 3.2 |
   | Abaqus/Standard | 5.0 (≤ 2,400 DOFs); 51 (≥ 48k DOFs) |
   | Wrap-up | 1.3 |
   | **Total** | **15.6 on small models** |

   So: "about 11 s fixed overhead plus Abaqus/Standard; about 16 s per job on small models".
2. **Phase splits of v1 runs.** v1 runs (pilot, `abss_before`) have wrong phase splits, because v1 predates the
   AM/PM fix. Their total times are correct. Do not use their `job_phase` columns.

## Final results (headline unchanged from the 22:30 update)

- **8 matched slots: FAFD faster in every case. No crossover.**
  - Family A elastic: 3–12×.
  - Increments: ABSS grows linearly with N, FAFD is almost flat. T_FAFD / T_ABSS at N = 1, 4, 16 is 0.49, 0.20 and
    0.06 (single runs).
  - Family B J2: 22–87×.
  - Ti-6242: 7–77×. At P = 56 (with inactive slots) the ratio is 0.56, the closest to parity.
- **1 slot each: measured crossover.** Replicated ABSS is faster for P ≥ 4 on meshes up to 2,400 DOFs. Brackets:
  2 < P\* < 4 at 432 DOFs; 2,400 < DOF\* < 15,552 at P = 4. Three interleaved repeats.
- **Before/after, identical cases:** 2–14× faster than the starting point; Ti P = 56: 6,362 s → 398 s (about 16×).
  Same derivatives.
- **Accuracy vs FAFD**, max |ABSS − FAFD| / (max|F| / |a|):
  - family A: 5e-9 to 7e-7;
  - family B: ≤ 2e-7, except one derivative flagged non-smooth by ABSS (7.2e-4);
  - Ti: ≤ 1.4e-7, all 132 SDVs included.
- **Step size:** both methods converge to the same derivative (A64, B16). ABSS is insensitive to h from 1e-4 to
  1e-6. FAFD's round-off grows at h = 1e-6.

## Not executed

- The serial per-branch mesh study (`A_mesh_j1`, dropped for time).
- Ti per-branch on the 3×3×3 cube.
- B beyond 7,007 DOFs.
- Repeats for the parameter-count, increments, B and Ti stages: these are single runs.

## Hypotheses (not measured)

- A crossover near P ≈ 100 for Ti at 8 slots.
- ABSS gains on strongly nonlinear analyses with many Newton iterations, or where perturbed reruns fail.
