# ABSS performance: profile, changes, verification

This document covers what an ABSS run spends its time on, what was changed to reduce it, and how each change was
shown to leave every result unchanged. The benchmark campaign that uses these changes is in
`benchmarks/nasa_stri/` (`REPORT.md`). All numbers here are measurements on the machine in
`benchmarks/nasa_stri/results/ENVIRONMENT.md`; every "verified" statement points to the run or test that shows it.

## 1. Where the time goes

### 1.1 One Abaqus job

ABSS uses Abaqus as a residual oracle, so a sensitivity run is many short Abaqus jobs. One job on an 8-element
model was timestamped line by line (`abaqus job=... interactive`):

| Phase | Seconds |
|---|---|
| Driver start-up, before the first output line | 9.6 |
| Licence checkout (FlexNet, 5 tokens) | 7.9 |
| Input processing (`pre.exe`) | 2.8 |
| Abaqus/Standard (`standard.exe`) | 2.8 |
| Wrap-up | 1.7 |

`abaqus information=release` alone takes 2.2–3.0 s. That single manual timing is not representative.

**Over the 6,065 Abaqus jobs of the campaign** (profiler `job.phase.*`; `benchmarks/nasa_stri/job_anatomy.py`):

| Phase | Seconds per job |
|---|---|
| Driver start-up + licence checkout | 6.3 |
| Input processing | 3.2 |
| Abaqus/Standard | 5.0 on models ≤ 2,400 free DOFs; 51 at ≥ 48k (includes its start-up, restart I/O and output) |
| Wrap-up | 1.3 |
| **Total** | **15.6 on small models; 20.9 overall** |

About 11 s of every job is fixed overhead outside the finite-element work, and it is the same for an ABSS job and an
FAFD analysis. **On small and medium models, the number of Abaqus jobs, and how many can run at once, decides the
run time.**

### 1.2 Profiler

`abss-cli run|fafd ... --profile run.json` records the following:
- **Elapsed time per phase:** setup, nominal, sensitivity, validation, serialization.
- **Summed worker time and counts:**
  - every Abaqus job, split into slot wait, driver + licence, input processing, analysis and wrap-up;
  - input generation, result parsing and file clean-up;
  - tangent assembly, factorization, direct solves, GMRES right-hand sides and GMRES products.
- **Attribution:** every job is credited to the activity that requested it (replay probe, tangent check, branch
  probe, tangent column group, GMRES product, commit, fused commit, validation rerun, FAFD analysis).
- **Per-increment diagnostics:** tangent check, assembled-tangent check, column groups, GMRES products and
  residual, re-probes, predictor consistency.

Summed worker time and elapsed time are kept apart: concurrent jobs add up in the former, not in the latter.

### 1.3 Baseline accounting (commit 3ef872b)

Order-1 sensitivities with respect to P parameters cost, per increment:
- 1 replay probe;
- 2 tangent-check probes;
- 2P branch probes;
- 1 replay commit and 2P branch commits;
- when the oracle tangent is inconsistent: 2 probes per free DOF (dense probed tangent, only when free DOFs ≤ 4P),
  or 2 probes per GMRES iteration per right-hand side.

That gives **4P + 4 jobs per increment**, in 5 sequential waves: replay probe, tangent check, branch probes, replay
commit, branch commits.

**Example: Ti-6242, 56 parameters, 12 increments (the earlier 106-minute run).** Per increment, 1 + 2 + 16 + 112 +
113 = 244 jobs, so 2,928 jobs in total. The 106 min (6,362 s engine) used 8 concurrent jobs. The earlier "44 min" for
FAFD was the validation: 113 complete analyses (56 × 2 plus a zero-offset rerun), run *serially* by the validation
code of that time. The "6 min parallel FAFD" was an estimate (113 analyses / 8 slots × 23 s) and was never measured.
The 106 vs 44 minute comparison therefore set parallel ABSS against serial FAFD. The campaign replaces it with
measured matched-slot comparisons. The validation also re-ran the zero offset. That run is not a duplicate of the
nominal: it puts the reference on the fixed grid and tight controls shared by the perturbed reruns, and it is kept.

## 2. Changes

Each change keeps every evaluated state, history, parameter value, boundary condition and tolerance. Only the
grouping of evaluations into Abaqus jobs and the order of jobs change, except for the tangent assembly in 2.7, which
is a different (verified) way of obtaining the same true tangent. Diagnostics, tolerances, outputs and requested
derivatives are untouched.

### 2.1 One probe wave and one commit wave per increment

The replay probe, the tangent-check pair and the order-1 branch probes all evaluate states known before any solve
(the order-1 predictor is the converged nominal state itself), so they now run as one wave. The replay commit and the
branch commits are independent histories and also run as one wave. That makes 2 waves per increment instead of 5,
with the same jobs. `--no-overlap` restores the old order.
*Verified:* the unit test "Concurrent waves and concurrent GMRES right-hand sides reproduce the serial engine
exactly" gives a maximum difference of 0. *Measured:* on the pilot 54-DOF block with 2 parameters and 8 slots,
the sensitivity stage fell from 80.3 s to 32.2 s.

### 2.2 Commit and next probe in one job (fused jobs)

A branch commit job for increment i now also runs a second step, the probe of increment i+1 at the nominal state
u\*<sub>i+1</sub>. That probe step writes no restart data (`*RESTART, WRITE, FREQUENCY=0`), and the next job restarts
from the end of the commit step (`*RESTART, READ, STEP=i, INC=1, END STEP`). The branch history is therefore exactly
what a separate probe job would have seen.
- **Restart mechanism:** verified with a restart from the end of step 1 of a job that continued into step 2 without
  restart data (J2 plasticity). Step 2 was reproduced to 9.1e-13 absolute (7e-16 relative); different states differ
  by 2.6e3 (`benchmarks/nasa_stri/filcmp.py`).
- **Whole run:** a 20-increment J2 load/unload run with a step change and a probed tangent, fused vs the old
  schedule, differs by **0** in every field (`results/fusion_check`).
- **Not bitwise in general:** the restart is equivalent to round-off, not to the bit. On the 3D plastic family-B
  case, where GMRES differences residuals at ε = 1e-6 |u|, that round-off moves the GMRES iterates by about the GMRES
  tolerance. The derivatives then differ from the old schedule by ≤ 2.0e-8 of the field's largest value (U 7.5e-10,
  RF 2.0e-8, S 1.7e-8, PEEQ 1.2e-10; `results/v4_check`). That is far below the finite-difference truncation error
  (~1e-5) and the validation tolerance (1e-3). The replicated and per-branch engines agree bitwise on the same case.
- **Fallback:** fusion is not used where the next step depends on the commit: dynamics, the reaction of a BC
  released by the next step, and NLGEOM rotation composition.

### 2.3 Tangent-check pair as sibling jobs

With concurrent jobs, the next increment's tangent-check pair runs in two sibling jobs. Each replays the replay
commit, writes no restart data, then probes. The next increment then needs no probe wave at all when the tangent is
consistent. The job count is unchanged (2 jobs either way). Each sibling's replayed commit is compared with the
primary commit at run time; a difference above 1e-10 relative stops the run.

### 2.4 No ODB output in branch jobs

Branch steps now end with `*OUTPUT, FIELD, FREQUENCY=0` and `*OUTPUT, HISTORY, FREQUENCY=0`. ABSS reads only the
`.fil` file. *Verified:* the `.fil` results are bitwise identical with and without it, and the ODB shrinks to the model
skeleton. `--odb-output` restores the default.

### 2.5 Validation reruns in parallel

The reference reruns are independent analyses and now run as one wave limited by the job slots. 9 reruns took 55 s
with 4 slots, against about 180 s serially. Validation cost is reported separately from the sensitivity cost.

### 2.6 GMRES right-hand sides side by side

The right-hand sides of one increment share the operator, and each GMRES product is 2 independent probe jobs.
Concurrent right-hand sides make the wall time follow the longest solve instead of the sum. Block or recycled Krylov
methods were not added: with ≤ 16 products per right-hand side measured (family B, increment 2: 16 products to 7.9e-9),
recycling has little to save, while concurrency removes the sequential cost.

### 2.7 True tangent from grouped probes (column colouring)

When the oracle tangent is inconsistent (built-in plasticity, the Ti-6242 UMAT), the old engine either probed the
tangent column by column (2 probes per free DOF, only when free DOFs ≤ 4P) or ran GMRES. It now assembles the true
tangent with Curtis–Powell–Reid column grouping on the oracle tangent's sparsity pattern:
- columns that share no row are perturbed together, so 2 probes per group recover each of their entries;
- all the probes are independent and run in one wave;
- on a hex mesh that is ≈ 2 × 81 probes, however large the mesh.

A cost model chooses between assembly and GMRES from the estimated Abaqus job rounds, using the job slots and the GMRES
products measured on the latest GMRES increment.

**Safeguards** (tightened after code review):
- **Complete pattern:** the pattern is the full element connectivity. Exact zeros of the element matrices are kept,
  because the true tangent may be nonzero there under plasticity.
- **Grouping only when safe:** grouping is used only when no constraints, contact, user elements or stiffness-free
  DOFs exist. Otherwise every column is probed alone and every row is read, which is exact but costs 2 probes per
  free DOF; the cost model weighs that against GMRES.
- **Row-by-row check:** each assembled tangent must reproduce the independent tangent-check probes row by row. Each
  row's error is scaled by that row's own size Σ|K<sub>qc</sub> v<sub>c</sub>|, floored at 1e-3 of the largest
  row, so small rows cannot hide behind large ones. Above 1e-4, GMRES is used and the log says so. *Verified:* exact recovery in the unit test (`linalg`). The plastic plate (7 increments)
and truss now use the assembled tangent, and every derivative validates against full reruns (6e-8 to 2e-6).

### 2.8 Replicated branches (`--batch`)

The history branches of a run become disjoint copies of the model, with shifted node and element labels and suffixed
set, surface, material and section names, inside one Abaqus job and one restart chain per job slot. A probe wave then
costs ⌈probes / replicas⌉ jobs per group, and a commit wave costs one job per group (with the next increment's probes
as a second step). The replay branch has 3 replicas (replay probe plus tangent-check pair) in its own group.

The supported decks are flat decks with the keywords listed in `src/oracle/abaqus_replica.hpp`. Anything else is
refused by name and the run falls back to one job per branch, saying so in the log.

*Verified:*
- **Abaqus copies:** two J2 copies in one job reproduce their single-model analyses bit for bit over 20 plastic
  increments (difference 0; the control differs by 4.8).
- **Whole runs:** `--batch` on family A (P = 4, 1 and 4 slots) matches the per-branch engine exactly: RF, S and E
  differ by 0, U by ≤ 1e-31 absolute (2e-26 relative).
- **Run-time guard:** replicas of one branch are compared at every commit and must agree to 1e-10.
- **User subroutines:** with a user subroutine, copies carry shifted element numbers (NOEL) and suffixed material
  names (CMNAME). A subroutine whose result depends on either would differ between copies. The replay branch's
  replicas would expose that at the first commit, and the log warns. The Ti-6242 bridge was verified bitwise.
- **Whole-model loads:** loads or BCs without a target set (whole-model loads) are refused for replication when the
  groups are formed, and the run falls back to one job per branch.

### 2.9 FAFD backend and comparison tool

`abss-cli fafd` is **the baseline, not ABSS**. It runs 2 complete analyses per parameter (central differences) with
the same step h, the nominal's time grid, the same tight controls and the same outputs, concurrently up to `--jobs`.
Its results file is labelled "FAFD (full-analysis finite differences)". `abss-cli compare a.abss b.abss` reports, per
derivative and field, max |ref|, max |error| and the scaled error over every increment.

### 2.10 Examined and not changed

- **A persistent Abaqus process that evaluates residuals on request.** Abaqus/Standard has no supported interface
  for this, and none was attempted. Every evaluation in ABSS remains a documented Abaqus job.
- **Driver start-up, licence checkout, input processing and wrap-up (≈ 11 s per job, measured over 6,065 jobs).**
  These are outside ABSS. The changes reduce how many times they are paid.
- **Output volume of commit jobs.** The commit fields (S, E, PE, SDV, ...) are what ABSS differentiates. They are
  kept.
- **The zero-offset validation rerun.** It is not a duplicate (see 1.3).

## 3. Before and after

See `benchmarks/nasa_stri/REPORT.md`, section "Before and after", for identical cases run with the v1 binary
(`--no-overlap`, the original schedule) and the current binary.
