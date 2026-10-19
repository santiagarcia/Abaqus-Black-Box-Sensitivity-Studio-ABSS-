# Abaqus Black-Box Sensitivity Studio (ABSS)

ABSS is a native C++20 / Qt 6 / VTK / Kokkos desktop application. It computes how **every result of an Abaqus/Standard
analysis, at every accepted increment, changes when a model parameter changes**. It supports first- and second-order
derivatives with respect to material constants, opaque UMAT/UEL `PROPS`, loads, boundary conditions, section
properties and geometry. You open one INP file, pick parameters from an automatically discovered list, choose the
derivative order and press **Run**. The results are then explored in the same window as 2D/3D colour maps and
history charts.

ABSS does **not** re-implement any Abaqus element or material. Abaqus is used as an opaque *residual and tangent
oracle*. ABSS prescribes displacements, reads reaction forces (`r = −RF`), replays perturbed load histories through
restarts, and solves the history-consistent sensitivity equations itself (see [docs/SCIENCE.md](docs/SCIENCE.md)).
Everything runs locally. Nothing is uploaded anywhere.

![Results view](docs/screenshots/explore_plate.png)

## What is verified, and what is not

| Capability | Status | Evidence |
|---|---|---|
| Sensitivity engine (orders 1–2; history-consistent probe/commit branches) on synthetic J2-plasticity models (truss, plane-strain plate, 3D hexahedral cantilever) with loading, unloading and reloading | **Validated**: every increment and every field (U, RF, S, E, PEEQ, Mises) against full reruns, typically 1e-6 to 1e-8 relative | `ctest`, [VALIDATION_RESULTS](docs/VALIDATION_RESULTS.md) §2 |
| Analytic checks (truss): `du/dE`, `d²u/dE²`, `d³u/dE³`, `du/dF`, parameter-dependent prescribed BC, reaction sensitivity | **Validated** (synthetic, exact formulas) | `test_engine.cpp` |
| Mixed second derivatives (∂²/∂a∂b, by polarization) | **Validated** on the J2 plate against 4-corner reruns (4.6e-7) | `test_mixed.cpp` |
| Inconsistent tangent (Abaqus gives the elastic K under plasticity) handled with matrix-free GMRES | **Validated** on synthetic J2 models with a deliberately elastic tangent; a negative control fails without it | `test_krylov.cpp` |
| **Real Abaqus 2024**: truss (T2D2) `du/dE`, `d²u/dE²`, `du/dF` against analytic formulas; tangent check | **Validated** | ctest label `abaqus`, VALIDATION_RESULTS §3 |
| **Real Abaqus 2024**: CPS4 J2 plasticity, 20 increments with load/unload; E, σ_y, prescribed displacement, order 1 | **Validated**: every increment and every field against full Abaqus reruns (5.0e-4, 3.2e-4, 1.6e-5). Abaqus's elastic tangent was detected at all 11 plastic increments and corrected with GMRES | VALIDATION_RESULTS §3.3 |
| Real Abaqus order 2 beyond the elastic truss | not yet run (cost: about 3× the order-1 jobs) | ABAQUS_REQUIRED_TESTS |
| Order 3 | **Experimental** (exact on the analytic truss; not validated on real Abaqus) | |
| UMAT / UEL (opaque PROPS) | Implemented (`--user`); **not run** (no Intel Fortran on the development machine) | [ABAQUS_REQUIRED_TESTS](docs/ABAQUS_REQUIRED_TESTS.md) B1–B4 |
| MPC/TIE/coupling, contact, dynamics, RIKS, parts/instances in the Abaqus backend | **Refused with a message** | [KNOWN_LIMITATIONS](docs/KNOWN_LIMITATIONS.md) |

Every result in the app carries its own evidence badge: **Validated**, **Experimental**, **Unverified**, **Failed**
or **Synthetic**. The badge comes with notes, including suspect increments flagged by the non-smoothness detector.

## Installation (Windows, as used for development)

1. **MSYS2 UCRT64** (user-local install, no admin needed):
   ```powershell
   $tc = "$env:USERPROFILE\toolchains"
   Invoke-WebRequest https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe -OutFile $tc\msys2.sfx.exe
   & $tc\msys2.sfx.exe -y "-o$tc\"
   & $tc\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"
   & $tc\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,qt6-base,qt6-charts,qt6-svg,vtk,eigen3,suitesparse,nlohmann-json,doctest}"
   ```
2. **Kokkos with OpenMP.** The MSYS2/Trilinos Kokkos is Serial-only, so build Kokkos yourself:
   ```powershell
   & $tc\msys64\usr\bin\bash.exe -lc 'export PATH=/ucrt64/bin:$PATH; cd /c/Users/$USER/toolchains && curl -LO https://github.com/kokkos/kokkos/releases/download/5.2.2/kokkos-5.2.2.tar.gz && tar xzf kokkos-5.2.2.tar.gz && cmake -S kokkos-5.2.2 -B kokkos-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 -DKokkos_ENABLE_OPENMP=ON -DCMAKE_INSTALL_PREFIX=$HOME/../../c/Users/$USER/toolchains/kokkos-openmp && cmake --build kokkos-build && cmake --install kokkos-build'
   ```
   The toolchain file picks up `%USERPROFILE%\toolchains\kokkos-openmp` automatically. Without Kokkos, ABSS builds
   with serial CPU kernels (`-DABSS_WITH_KOKKOS=OFF`).
3. **Abaqus** (optional): only needed for new analyses. ABSS finds `C:\SIMULIA\Commands\abaqus.bat`, or set
   `ABSS_ABAQUS_CMD`.

Linux: `scripts/build_linux.sh` (needs Qt6, VTK with GUISupportQt, Eigen, nlohmann_json, doctest; optional
SuiteSparse and Kokkos).

## Build and test

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1          # configure + build + ctest (excludes the Abaqus test)
```
or manually (PowerShell needs the quotes around `-D` arguments):
```powershell
$env:PATH = "$env:USERPROFILE\toolchains\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=cmake/msys2-ucrt64.cmake"
cmake --build build
ctest --test-dir build -LE abaqus --output-on-failure   # all tests that do not need Abaqus
ctest --test-dir build -L abaqus --output-on-failure    # real Abaqus test (~2 min, uses licence tokens)
```
Package a self-contained folder and zip (all DLLs, Qt plugins, examples, docs): `scripts\package.ps1`.

## First launch and demo walkthrough

```powershell
$env:PATH = "$env:USERPROFILE\toolchains\msys64\ucrt64\bin;$env:PATH"
.\build\bin\abss.exe
```
1. **Start**: shows whether Abaqus was found ("Check now" tests the licence with a one-element job), the compute
   backend and the sparse solvers. Choose *Plate with a hole* and press **Run demo and show results**. No Abaqus is
   needed; the demo is a built-in synthetic J2 model and is labelled as such everywhere.
2. **Parameters**: a searchable table grouped by category, with confidence badges (known meaning vs. opaque
   `PROPS[k]`) and the source `file:line` of every value.
3. **Set up**: the derivative order (0–2; 3 is marked Experimental), outputs, "Validate against full reruns", and a
   cost estimate (oracle evaluations / Abaqus jobs).
4. **Run**: progress, a live log and Cancel. The interface stays responsive (the work runs on a worker thread).
5. **Results**: choose the output (U2, S11, Mises, PEEQ…), the parameter and the order (for example `dU2/dE`).
   - **Colouring:** palettes, a diverging range centred at zero for derivatives, and range modes (all increments,
     this increment, fixed). Integration-point data is shown either as an element average, labelled as a
     projection, or at a single chosen point.
   - **History:** click points on the mesh to plot histories over all increments; Ctrl+click compares points.
   - **Export:** CSV, PNG and VTU. Save as `.abss`, which reopens without Abaqus.

Unattended example: `abss.exe --demo plate --run --pick 2 --screenshot shot.png --quit`.

## Running a real Abaqus model

GUI: **File → Open** an `.inp`, select parameters, then Run. CLI:
```powershell
.\build\bin\abss-cli.exe inspect examples\models\truss.inp                  # list parameters and their ids
.\build\bin\abss-cli.exe run examples\models\truss.inp --params "mat:STEEL:elastic:E,load:LOAD:cload:TIP:1" `
     --order 2 --jobs 4 --validate --out truss.abss
.\build\bin\abss-cli.exe show truss.abss                                    # evidence summary, no Abaqus needed
```
- **Inputs.** Requirements in this version: a flat deck (no parts/instances) with `*STATIC` steps and
  `*BOUNDARY`/`*CLOAD`/`*DLOAD`/`*DSLOAD` loading. The original INP is never modified; edited copies live in the
  scratch folder.
- **User subroutines.** For a UMAT/UEL model, pass `--user my_umat.f` (or `.obj`). PROPS are differentiated as opaque
  indexed constants.
- **Cost.** Every probe and commit is a short Abaqus job (about 13 s of start-up on the development PC). `--jobs N`
  runs N at once (licence permitting). See SCIENCE.md §8 for the job anatomy.

## Repository layout
`include/abss`, `src/{core,inp,linalg,oracle,sensitivity,viz,gui}`, `apps/{abss,cli}`, `tests/{core,inp,linalg,oracle,sensitivity,gui,abaqus}`,
`examples/{models,subroutines}`, `validation/` (Council experiments and reference models), `docs/`, `scripts/`,
`cmake/`, `reference/` (the original Plotly viewer, kept for reference only).

## Documentation
[SCIENCE](docs/SCIENCE.md) · [ARCHITECTURE](docs/ARCHITECTURE.md) · [VALIDATION_RESULTS](docs/VALIDATION_RESULTS.md) ·
[KNOWN_LIMITATIONS](docs/KNOWN_LIMITATIONS.md) · [ABAQUS_REQUIRED_TESTS](docs/ABAQUS_REQUIRED_TESTS.md) ·
[COUNCIL_REVIEW](docs/COUNCIL_REVIEW.md)
