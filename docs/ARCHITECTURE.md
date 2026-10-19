# ABSS architecture

ABSS is a C++20 / CMake project. The numerical core is Qt-free and headless-testable; Qt 6 and VTK are used only by the
visualization and GUI layers.

```
 apps/abss (Qt GUI)      apps/cli (abss-cli)
        │                        │
   abss_gui ── abss_viz (VTK)    │
        │                        │
        └──────── abss_sensitivity ── engine.cpp (probe/commit recursion, mixed by polarization)
                        │              validation.cpp (full-rerun FD, corner reruns)
                        │              stencil.cpp (Fornberg weights, step policy, cost model)
                  abss_oracle ── Oracle / HistoryBranch interface (oracle.hpp)
                        │        ├─ SyntheticOracle (demo + test bed: small-strain J2 FE, exact tangent)
                        │        └─ Abaqus oracle (abaqus_*.cpp + scripts/abaqus/*.py)
              ┌─────────┼──────────┐
          abss_inp   abss_linalg   abss_core
     (deck parser,  (CSR, Eigen LU/   (Mesh, ResultStore, ParameterInfo,
      discovery,     LDLT, UMFPACK,    DerivativeSpec, QualityRecord,
      token patch)   Kokkos kernels)   .abss project I/O, SHA-256)
```

| Module | Directory | Responsibility |
|---|---|---|
| `abss_core` | `src/core`, `include/abss/core` | Mesh with Abaqus identities (instance, label), integration/section point layout (never averaged in storage), frames (step, increment, step/total time), fields, parameters with exact source provenance, derivative series keyed by `DerivativeSpec` (nominal, pure order k, mixed multi-index), measured quality evidence, `.abss` results file (JSON manifest + float64 payload), CSV history export. |
| `abss_inp` | `src/inp` | Structure-preserving INP model: verbatim lines, includes with provenance, keyword blocks, parts/instances flattening, sets, steps, boundaries; parameter discovery (materials, `*USER MATERIAL` PROPS as opaque indices, `*UEL PROPERTY`, loads, BCs, sections, amplitudes, node-set geometry); token-level edits that leave every other byte identical. |
| `abss_linalg` | `src/linalg` | CSR matrices, sub-blocking K_FF, factorization objects reused for all right-hand sides of an increment (Eigen SparseLU / SimplicialLDLT, SuiteSparse UMFPACK, dense LU for validation), Kokkos kernels (stencil combination over field arrays, SpMV/SpMM, reductions, Jacobi-PCG) each with a serial reference path. No explicit inverse anywhere. |
| `abss_oracle` | `src/oracle` | The black-box interface: `runNominal`, `tangent(i)`, `openBranch(delta)` → `HistoryBranch::residuals(i, u)` (probe, history untouched) and `commit(i, u)` (advance history, return fields), `solvePerturbed(delta)` (validation reference). Implementations: synthetic demo model and the Abaqus backend (job generation, safe subprocesses, result extraction). |
| `abss_sensitivity` | `src/sensitivity` | Order-K history-consistent recursion over every accepted increment, adaptive re-probe, non-smoothness detector, mixed derivatives by polarization, validation against full reruns. |
| `abss_viz`, `abss_gui` | `src/viz`, `src/gui` | VTK mixed-element grids, colour maps, picking, Qt main window (workflow rail, run worker thread, explore view, history charts). |

## Threading and responsiveness
The engine runs in a worker thread; the GUI receives progress through queued signals and cancels through
`RunControl::cancel` (checked between increments and orders; a cancelled increment is never stored). Branch probes
and commits may run concurrently (`parallelBranches`) when the oracle allows it; Abaqus job concurrency is limited by
`AbaqusRunConfig::maxConcurrentJobs` (licence tokens).

## Dependencies (Windows build used for development)
MSYS2 UCRT64 (GCC 15, CMake, Ninja): Qt 6.11, VTK 9.6.2, Eigen 5, SuiteSparse 7.14 (UMFPACK 6.3.8), nlohmann_json,
doctest; Kokkos 5.2.2 built from source with the OpenMP backend (the MSYS2/Trilinos Kokkos is Serial-only).
CUDA/HIP backends are selected by building Kokkos with them; ABSS code uses only `Kokkos::DefaultHostExecutionSpace`
today, see KNOWN_LIMITATIONS.
