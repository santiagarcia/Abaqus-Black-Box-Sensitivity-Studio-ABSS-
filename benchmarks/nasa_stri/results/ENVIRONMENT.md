# Benchmark environment and version record

Recorded on 2026-10-21. Every run's JSON also stores its own command line, input SHA-256, concurrency, parameter
ids, steps and profile.

## Hardware and OS

| Item | Value |
|---|---|
| CPU | Intel Core Ultra 9 285K: 24 cores, 24 hardware threads, 3.7 GHz base |
| Memory | 127.5 GB |
| Storage | Samsung PM9F1 2 TB NVMe SSD (scratch directories in `%TEMP%`) |
| OS | Windows 11 Enterprise 10.0.26200 |

## Abaqus and licence

| Item | Value |
|---|---|
| Abaqus | Abaqus 2024 (`C:\SIMULIA\EstProducts\2024`, build 2023_09_21-07.55.25 RELr426 190762) |
| Licence | FlexNet server `EB137960.utsarr.net`, shared site pool of 314 tokens. Each Abaqus/Standard job with `cpus=1` checks out 5 tokens. 8 concurrent jobs therefore hold 40 tokens. |
| Job options | `interactive ask_delete=OFF cpus=1` for every job, for both methods |

**Measured job anatomy** over 6,065 campaign jobs (`job_anatomy.py`):

| Phase | Seconds per job |
|---|---|
| Driver + licence | 6.3 |
| Input processing | 3.2 |
| Abaqus/Standard | 5.0 (≤ 2,400 free DOFs) to 51 (≥ 48k free DOFs) |
| Wrap-up | 1.3 |
| **Total** | 15.6 on small models |

The overhead is the same for an ABSS job and an FAFD job. A single manual timing of one job, before the campaign,
gave 17.5 s for driver + licence; the campaign average replaces it.

## Software under test

| Item | Value |
|---|---|
| Baseline commit | `3ef872b8f9882897c16bb9a1190bc842032ba852` |
| Compiler | g++ 16.2.0 (MSYS2 UCRT64), `-O3 -DNDEBUG -std=c++20` |
| Build | CMake 4.4.4, Ninja, Release |
| Kokkos | 5.2.2, OpenMP backend, 24 threads |
| Sparse direct | SuiteSparse UMFPACK |

Frozen binary snapshots used for measurement (`%LOCALAPPDATA%\abss_bench_bin\<tag>`):

| Tag | Content |
|---|---|
| v1 | Baseline commit plus profiling instrumentation. `--no-overlap` reproduces the original stage-by-stage schedule exactly. |
| v3 | v1 plus concurrent waves, fused commit+probe jobs, sibling tangent-check jobs, suppressed ODB output in branch jobs, parallel validation reruns, concurrent GMRES right-hand sides, coloured true-tangent assembly with its cost model, and the AM/PM profiler fix. |
| v5 | v3 plus replicated branches (`--batch`), the oracle wave API, and the plain nominal analysis in FAFD mode. Used for the family-A stages, which have a consistent tangent, so the v6 fixes do not apply to them. |
| v6 | v5 plus code-review fixes, used for families B and C and the before/after stage. The assembled true tangent uses a complete element pattern (exact zeros kept), and column grouping applies only when no constraints, contact or stiffness-free DOFs exist; otherwise every column is probed on its own. The assembled-tangent check is row-scaled. Commit steps always write restart data explicitly. Replica groups get lifetime and thread-safety fixes. Loads with an empty target are refused for replication. FAFD and `compare` skip the initial state. |

| v7 | v6 plus the SDV `.fil` reader fix (consecutive SDV records concatenated). |

**Binary per stage:**

| Binary | Stages |
|---|---|
| v5 | A_mesh_j8 (including the A96 re-run), A_params_j8 |
| v6 | B_mesh_j8, before_after (the "abss_before" runs use v1) |
| v7 | serial_crossover, h_study, A_inc_j8, C_ti |

The v6 and v7 changes do not touch the paths that family A exercises: consistent tangent, and no SDV.

Builds during the campaign are logged in `BUILD_WINDOWS.txt`. Runs that overlap a build window are flagged.

## Solver settings, identical for both methods

- **Nominal analysis and every FAFD analysis:** ABSS's tight controls (`*CONTROLS, FIELD` with R_n = 1e-8, plus relaxed
  iteration limits).
- **FAFD time grid:** each FAFD analysis is forced onto the nominal's accepted increments (`DIRECT`, `EXTRAPOLATION=NO`).
- **Derivatives:** first order, central differences, the same step h for both methods (the engine default,
  h_rel = 1e-5).
- **Outputs:** U, RF, S, E/LE, PE/PEEQ and SDV wherever the model has them, at every increment and every location.

## Material library (family C; private)

The Ti-6242 library is identified by its SHA-256 in the private results (`validation/private/`, not in git).
The library is never copied into any package or repository.
