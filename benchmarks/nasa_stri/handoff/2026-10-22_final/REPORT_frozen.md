# ABSS vs full-analysis finite differences: performance and benchmark report

**Question.** Under which measured conditions does ABSS give accurate sensitivities at a cost that competes with
full-analysis finite differences (FAFD)?

**Answer, measured on 2026-10-21 (Abaqus 2024, 24-core workstation, remote FlexNet licence):**

1. **With equal concurrency of 8 Abaqus jobs, FAFD was faster in every case.** That covers elastic meshes from 432 to
   110,976 free DOFs, 1 to 33 parameters, J2 load/unload plasticity, and the Ti-6242 crystal-plasticity UMAT.
   There is no crossover at 8 slots.
   - Elastic models: FAFD was 3–12× faster.
   - J2 plasticity: FAFD was 20–90× faster.
   - Ti-6242: FAFD was 7–77× faster, and 1.8× at 56 parameters.
   - The gap does not close with mesh size. It narrows with parameter count.
2. **With one Abaqus job at a time, ABSS with replicated branches (`--batch`) was faster** from P ≈ 4 parameters
   upward on small and medium meshes. This was measured with 3 interleaved repeats and bracketed:
   - between P = 2 and P = 4 at 432 free DOFs;
   - between 2,400 and 15,552 free DOFs at P = 4.
   - Mechanism: replicated ABSS needs N + 1 Abaqus jobs (N increments) whatever P is, while FAFD needs 2P.
3. **Accuracy:** ABSS agrees with FAFD to 5e-9 to 7e-7 of each derivative's natural size max|F|/|a|, over every
   increment and location. Two exceptions:
   - one derivative that ABSS itself flagged as non-smooth (yield onset inside the FD stencil): 7e-4;
   - none other.
4. **The optimizations made ABSS 2–14× faster than its starting point** on identical cases, with the same results.
   The largest gain is with few job slots.

A note for readers: every number below is a measurement. "Single run" marks values without repeats. Anything not
measured is labelled as a hypothesis.

## 1. Setup

- **Machine:** Intel Core Ultra 9 285K (24 cores), 127.5 GB, NVMe, Windows 11.
- **Abaqus:** 2024 (RELr426), FlexNet licence server on the network, 5 tokens per job, `cpus=1` for every job.
- **ABSS:** commit 3ef872b plus the changes in `docs/PERFORMANCE.md`, in frozen binaries v5–v7
  (`results/ENVIRONMENT.md`).
- **Fairness:**
  - same model, parameters and derivative order (1);
  - central differences with the same step (h = 1e-5·|a|);
  - the same fixed time grid (FAFD analyses are forced onto the nominal's increments);
  - the same tight equilibrium controls (R_n = 1e-8) and the same output fields;
  - equal job slots, 1 CPU per job.
- **Time definitions:** "sensitivity" is the sensitivity stage alone. "Workflow" is each method's own nominal
  analysis plus its sensitivity stage. FAFD's nominal is one plain analysis; ABSS's nominal also writes element
  matrices.
- **Validation reruns:** reported separately and not included in any timing comparison.
- **Concurrency:** parallel ABSS is never compared with serial FAFD.
- **Methods:**
  - `abss`: one Abaqus job per branch evaluation;
  - `abss_batch`: replicated branches, several branches as disjoint model copies in one job per slot;
  - `fafd`: the baseline;
  - `abss_before`: the original schedule (v1 binary).

## 2. Where the time goes (profiling)

**Average Abaqus job**, over the 6,065 jobs of the campaign (profiler phases; `job_anatomy.py`):

| Phase | Seconds per job |
|---|---|
| Driver + licence | 6.3 |
| Input processing | 3.2 |
| Abaqus/Standard | 5.0 on models ≤ 2,400 free DOFs; 51 at ≥ 48k |
| Wrap-up | 1.3 |
| **Total** | **15.6 on small models; 20.9 overall** |

About 11 s per job is fixed overhead outside the finite-element work, the same for both methods.

**Representative case, A48** (7,056 free DOFs, P = 4, 8 slots, repeat 2), per job:

| Run | Jobs (incl. nominal) | Sensitivity wall | Driver + licence | Pre | Standard | Wrap-up |
|---|---|---|---|---|---|---|
| ABSS replicated | 41 | 111 s | 8.1 | 3.1 | 6.9 | 1.3 |
| ABSS per-branch | 54 | 230 s | 9.3 | 3.1 | 6.4 | 1.4 |
| FAFD | 10 | 22 s | 12.3 | 3.4 | 7.2 | 0.9 |

ABSS's own time in the replicated run: 12.7 s reading results, 0.5 s writing inputs, 0.6 s of linear algebra. The
per-branch run spent 224 s of summed thread time waiting for a free slot. The phase split of v1-binary runs (pilot,
"before") is not used: that binary predates the AM/PM timestamp fix. Their total times are valid.

ABSS's own computation is small:
- **A48, P = 4, 8 slots, replicated:** about 14 s of summed worker time (13 s of it reading Abaqus result files,
  0.6 s of linear algebra) within a 101–111 s sensitivity stage. The rest is Abaqus jobs.

**Job counts, P = 4, 4 increments:**

| Method | Abaqus jobs |
|---|---|
| ABSS per-branch (fused commit + next-probe jobs) | 53 |
| ABSS replicated | 40 at 8 slots (5 waves × 8 groups), 5 at 1 slot |
| FAFD | 8 |
| ABSS before the changes | 80 |

**What grows with mesh size:** every ABSS job restarts from the branch's full saved state and writes it back
(143 MB of `.mdl` alone per job at 111k DOFs), plus the full output fields. An elastic FAFD increment is one sparse
solve. Measured consequence: ABSS's per-job time grows faster with mesh size than FAFD's.

## 3. Results

### 3.1 Family A, elastic cantilever, mesh scaling

Setup: P = 4, 4 increments, 8 slots, 2 interleaved repeats. Sensitivity stage in seconds, both repeats.

| Free DOFs | ABSS replicated | ABSS per-branch | FAFD | T_FAFD / T_ABSS replicated |
|---|---|---|---|---|
| 432 | 72.2 / 73.1 | 174.8 / 161.2 | 15.0 / 14.5 | 0.20 |
| 2,400 | 83.1 / 81.9 | 163.4 / 178.3 | 17.3 / 15.7 | 0.20 |
| 7,056 | 100.6 / 111.1 | 219.5 / 229.6 | 17.8 / 22.2 | 0.19 |
| 15,552 | 155.3 / 144.5 | 256.3 / 178.5 | 22.7 / 20.9 | 0.15 |
| 48,672 | 472.3 / 459.7 | 451.4 / 431.0 (r1 re-run after a stale-scratch failure) | 48.0 / 39.0 | 0.09 |
| 110,976 | 1755.8 / 1772.4 | 1833.1 / 2111.7 | 228.0 / 158.8 | 0.11 |

No crossover. Figures:
- `figures/A_time_vs_dofs_j8`, `figures/A_speedup_vs_dofs_j8`;
- `figures/A_breakdown_j8`, `figures/A_jobs_vs_dofs`;
- `figures/A_accuracy_vs_dofs`.

### 3.2 Family A, parameter count

Setup: 15,552 free DOFs, 4 increments, 8 slots, single runs. Sensitivity stage in seconds.

| P | ABSS replicated | ABSS per-branch | FAFD | Best T_FAFD / T_ABSS |
|---|---|---|---|---|
| 1 | 146 | 105 | 20 | 0.19 |
| 4 | 146 | 183 | 24 | 0.16 |
| 16 | 261 | 521 | 85 | 0.33 |
| 33 | 597 | 944 | 190 | 0.32 |

The gap narrows with P under replication but does not close at 8 slots. The parameters are:
- P = 16: E of all 16 segments;
- P = 33: also ν of all 16 segments and the pressure.

### 3.2b Family A, number of increments (secondary study)

Setup: 2,400 free DOFs, P = 4, 8 slots, single runs. Sensitivity stage in seconds.

| Increments | ABSS replicated | ABSS per-branch | FAFD | T_FAFD / T_ABSS replicated |
|---|---|---|---|---|
| 1 | 33.2 | 61.3 | 16.2 | 0.49 |
| 4 | 83.7 | 162.6 | 16.4 | 0.20 |
| 16 | 349.4 | 593.8 | 19.7 | 0.06 |

ABSS cost grows linearly with the number of increments, because it works increment by increment. An extra elastic
increment costs FAFD almost nothing. More increments therefore favour FAFD.

### 3.3 One Abaqus job at a time: the measured crossover

Setup: 1 slot each, 3 interleaved repeats. Sensitivity stage in seconds; the ratio is the median T_FAFD / T_ABSS
replicated, with the range over repeat pairings.

| Case | ABSS replicated | FAFD | Ratio (range) |
|---|---|---|---|
| 432 DOFs, P = 1 | 67 / 72 / 74 | 24 / 26 / 36 | 0.36 (0.33–0.54) |
| 432 DOFs, P = 2 | 75 / 79 / 98 | 53 / 59 / 65 | 0.74 (0.54–0.87) |
| 432 DOFs, P = 4 | 72 / 76 / 78 | 106 / 114 / 118 | **1.49 (1.36–1.64)** |
| 432 DOFs, P = 8 | 74 / 76 / 78 | 210 / 216 / 218 | **2.83 (2.70–2.94)** |
| 2,400 DOFs, P = 4 | 96 / 102 / 103 | 124 / 125 / 132 | **1.23 (1.20–1.38)** |
| 15,552 DOFs, P = 4 | 311 / 312 / 320 | 147 / 157 / 170 | 0.50 (0.46–0.55) |

**Crossover (measured, bracketed):**
- 2 < P\* < 4 at 432 DOFs and 4 increments;
- 2,400 < DOF\* < 15,552 at P = 4.

Replicated ABSS time is flat in P (72–78 s for P = 1 to 8) while FAFD grows linearly. Accuracy is 5e-9 to 2e-8.
Figures: `figures/S_*`.

### 3.4 Family B, J2 plasticity, load and unload

Setup: P = 4 (initial yield stress of segments 1–4), 8 increments, 8 slots, single runs. Sensitivity stage in seconds.

| Free DOFs | ABSS replicated | ABSS per-branch | FAFD | GMRES products (ABSS) |
|---|---|---|---|---|
| 423 | 355 | 689 | 16 | 0–112 |
| 2,375 | 834 | 1161 | 15 | 73–182 |
| 7,007 | 1708 | 1486 | 20 | 151 |

Built-in plasticity makes Abaqus export the elastic (unloading) stiffness, so ABSS must correct the tangent at every
plastic increment. It uses either the true tangent assembled from grouped probes (81 column groups) or GMRES (65–86
products per increment). That correction dominates the cost. Every assembled tangent is checked; where one failed
the check (2.2e-3 > 1e-4), GMRES was used, as designed.

### 3.5 Before and after, identical cases

Single runs. Sensitivity stage in seconds.

| Case | Before (original schedule) | Per-branch now | Replicated now | FAFD |
|---|---|---|---|---|
| A16, P = 4, 1 slot | 1074.6 | 756.1 | **78.4** (13.7×) | 106.5 |
| A16, P = 4, 8 slots | 224.9 | 150.2 | **71.6** (3.1×) | 14.7 |
| B16, P = 1, 8 slots | 823.7 | 510.8 | **354.2** (2.3×) | 14.4 |
| c3d8 block, P = 2, 8 slots | 55.3 | 30.4 | **27.0** (2.0×) | 16.0 |

The derivatives are unchanged:
- schedule changes are bitwise identical where the restart path is unchanged;
- fused restarts agree to ≤ 2e-8 of the field scale (`docs/PERFORMANCE.md` §2.2).

### 3.6 Family C, Ti-6242 compiled UMAT (private model; detailed files are not in the repository)

Setup: unit cube refined n × n × n, with the same library, properties, symmetry BCs, stretch and 12 NLGEOM increments.
8 slots, single runs. Sensitivity stage in seconds.

**Parameter sets:**
- P = 1: PROPS[47];
- P = 4: the sum-preserving linked direction (+t on PROPS 1, −t on PROPS 2) plus PROPS 47, 34 and 30;
- P = 13: the linked direction plus the 12 parameters active on this path.

| Mesh (free DOFs) | P | ABSS replicated | ABSS per-branch | FAFD |
|---|---|---|---|---|
| 1 element (8) | 1 | 349 | 518 | 16 |
| 1 element (8) | 4 | 377 | 703 | 15 |
| 1 element (8) | 13 | 385 | 1121 | 57 |
| 8 elements (45) | 1 | 966 | 2386 | 15 |
| 8 elements (45) | 4 | 997 | 2610 | 16 |
| 27 elements (128) | 1 | 1457 | not run | 19 |

**56-parameter case (before/after; includes 44 slots that are inactive on this path):**

| Run | Sensitivity stage |
|---|---|
| ABSS, original engine (earlier run) | 6,362 s |
| ABSS replicated now | 398 s (16× faster) |
| FAFD at matched 8 slots (measured) | 223 s |

The earlier "≈ 6 min parallel FAFD" was an estimate. T_FAFD / T_ABSS = 0.56 is the closest to parity measured at
8 slots.

**Hypothesis, not measured.** Replicated ABSS time is nearly flat in P here (377–398 s for P = 4–56). A crossover near
P ≈ 100 would follow, but no such case was run.

The UMAT's tangent is inconsistent at every increment (check 2e-3 to 7e-2), so ABSS assembles the true tangent from
grouped probes at every increment (11–12 of 12). That, together with the per-increment job structure, sets its cost.

**Accuracy against FAFD**, every field including all 132 SDVs: 4.7e-8 to 1.4e-7 of the natural scale, ≤ 1.6e-6 with
the validation floor.

## 4. Accuracy throughout

The metric is max |ABSS − FAFD| over every increment and location, divided by the derivative's natural size
max|F_nominal| / |a|. Dividing by max|FAFD| instead would be meaningless where the true derivative is ≈ 0, for
example a root reaction against the stiffness of a far segment: there FAFD returns only its own noise, about 1e-10.
Both ratios, and a version with ABSS's validation floor, are in `acc_*.json`.

| Family | Worst error | Worst field |
|---|---|---|
| A, mesh | 5e-9 to 4e-8 | S |
| A, parameters | ≤ 7e-7 | U, ν of segment 15 |
| B | 7.6e-9 to 2.1e-7, except one flagged derivative | |
| Ti, every case, including 132 SDVs | 4.7e-8 to 1.4e-7 | |

**Non-smooth event (family B, 7,007 DOFs, segment-3 yield stress).** The worst error was 7.2e-4. ABSS flagged that
derivative itself: its predictor-consistency diagnostic was 2.2e-3 at increment 2, against about 1e-5 elsewhere,
because segment 3 starts yielding inside the ±h stencil there. The derivative is one-sided at that increment. It is
reported as flagged, not as a pass.

**Step-size convergence** (`results/h_study`, P = 1, 8 slots). Reference: FAFD at h_rel = 1e-3.

| | ABSS, h = 1e-4…1e-6 | FAFD, h = 1e-4…1e-5 | FAFD, h = 1e-6 |
|---|---|---|---|
| A64 elastic, worst field | 9.8e-7 | 9.7e-7 | 1.1e-6 |
| A64 elastic, RF | 2.5e-9 at every step | 2.9e-9 | 1.1e-7 |
| B16 plastic, worst field | 1.2e-7 | 1.2e-7 | 1.25e-7 |
| B16 plastic, U | 1.0e-9 | 6.5e-10 | 9.1e-9 |

- In each family, every run differs from the reference by the same amount: the reference's own O(h²) truncation.
  The methods otherwise agree with each other.
- ABSS is insensitive to h over two decades.
- FAFD's error grows at h = 1e-6, from round-off divided by 2h. This confirms that FAFD, not ABSS, is the noisier
  side where the two disagreed near zero.

This is derivative agreement, not convergence of the finite-element discretization. Mesh refinement changes the
derivative itself (§3.1 compares methods at each mesh).

## 5. Usefulness

- **Compatibility.** The same decks run unchanged under both methods. `--batch` needs flat decks without
  constraints or contact, and falls back to one job per branch otherwise, saying so.
- **Setup.** Both methods are one command, or a few clicks in the GUI.
- **What ABSS adds:**
  - a per-increment diagnostic trail: tangent consistency, predictor consistency, non-smooth-event flags,
    GMRES and assembled-tangent residuals;
  - built-in validation;
  - higher-order and mixed derivatives;
  - a GUI that shows every derivative field over the mesh and through the history.

  FAFD on the frozen grid also gives derivatives at every increment, so that is not unique to ABSS.
- **Resources.** Each method uses 5 licence tokens per running job. Replicated ABSS uses more memory and disk per
  job: up to 25 GB of restart files in scratch for the 111k-DOF case, removed after the run.
- **Runtime.** See §3.

**Recommendation.** For runs where several Abaqus jobs can execute at once, FAFD is the faster way to get these
first-order sensitivities on the models measured here. ABSS should offer FAFD as a clearly labelled automated
backend: `abss-cli fafd` exists, and its output is labelled FAFD, never ABSS. ABSS-replicated is the faster choice
when only one Abaqus job can run at a time and P ≥ 4 on small and medium meshes, and whenever its diagnostics or
higher-order derivatives are wanted.

## 6. Not executed, and limits of this study

- Repeats: 2 for family A mesh scaling; single runs for parameter count, family B, before/after and Ti; 3 for the
  serial crossover.
- Family B ran up to 7,007 DOFs only; Ti up to the 3×3×3 cube. Larger plastic meshes were not run for time.
- Not executed, for time:
  - the serial per-branch mesh study (`plans/A_mesh_j1.dropped.json`). Serial data exist for A16 per-branch
    (before/after) and for replicated ABSS against FAFD (§3.3).
  - Ti per-branch on the 3×3×3 cube.
- Increments study: single runs (§3.2b).
- Hypotheses not tested here:
  - analyses whose perturbed reruns fail or need different increments;
  - strongly nonlinear analyses with many Newton iterations per increment (family B has few);
  - a closer licence server, which would shrink the per-job overhead for both methods.

## 7. Errata found during this work

- **Ti-6242 SDVs.** The earlier "SDV validated" result compared zeros with zeros, because a `.fil` reader bug kept
  only the last SDV record. It is fixed, and the SDVs are now verified against FAFD.
- **SCIENCE.md validation floor.** The document said 1e-4; the code uses 0.05. The document is corrected.

## Files

- `results/<stage>/`: one JSON (profile and configuration), one log and one `.abss` per run; `acc_*.json` for
  accuracy.
- `handoff/`: frozen summaries.
- `figures/`: PDF, SVG and PNG.
- `plans/`: exact run definitions.
- Scripts: `gen_models.py`, `refine_cube.py`, `campaign.py`, `run_campaign.ps1`, `analyze.py`, `accuracy.py`,
  `figures.py`.
