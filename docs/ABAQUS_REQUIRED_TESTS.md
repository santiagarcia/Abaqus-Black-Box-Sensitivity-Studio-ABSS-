# Tests that need a licensed Abaqus

All commands are run from the repository root on Windows PowerShell, with the build from README §Build. On Linux,
replace `abss-cli.exe` with `abss-cli` and the Abaqus command path.

```powershell
$env:PATH = "$env:USERPROFILE\toolchains\msys64\ucrt64\bin;$env:PATH"
$cli = ".\build\bin\abss-cli.exe"
```

## A. Already run on the development machine (Abaqus 2024, build 2023_09_21)

| ID | What | Command | Status |
|---|---|---|---|
| A1 | Detect Abaqus and confirm a licence | `& $cli abaqus-info --check-licence $env:TEMP\abss_lic` | pass |
| A2 | T2D2 truss: analytic `du/dE`, `d²u/dE²`, `du/dF`, geometry; tangent check | `ctest --test-dir build -L abaqus --output-on-failure` | **pass** (129 s) |
| A3 | Truss with full-rerun validation, order 2 | `& $cli run examples\models\truss.inp --params "mat:STEEL:elastic:E,load:LOAD:cload:TIP:1,geom:TIP:X" --order 2 --jobs 4 --validate --out truss.abss` | **pass** (6/6 Validated, 449 s) |
| A4 | CPS4 J2 plasticity, 20 increments (load, then unload), order 1, every increment vs reruns | `& $cli run examples\models\j2_plastic.inp --params "mat:STEEL:elastic:E,mat:STEEL:plastic:sigma_y:1,bc:LOAD:RIGHT:1-1" --order 1 --jobs 6 --validate --out j2.abss` | **pass** (5.0e-4 / 3.2e-4 / 1.6e-5; 89 min) |
| A5 | Abaqus mechanism experiments (oracle sign, restart replay, tangent output, precision) | `validation/experiments/scout/*`, `validation/experiments/curie/*` (commands in those reports) | see the council reports |
| A6 | UMAT and UEL, each as source, `.obj` only and library only, order 1, against analytic values | `.\build\bin\test_abaqus.exe -tc="UMAT*,UEL*"` (needs ifort/ifx + MSVC) | **pass** (112 assertions, 1518 s; VALIDATION_RESULTS §3.5) |

## B. User subroutines: still to run

The user-subroutine runs need Intel Fortran (ifort or ifx) and the MSVC linker; the ABSS installer can install both.
A6 already covers the order-1 analytic checks for all three delivery forms.

| ID | What | Command | Pass criterion |
|---|---|---|---|
| B1 | Opaque UMAT, order 2, with full-rerun validation | `& $cli run examples\models\umat_props.inp --params "umat:UMAT_MAT:props:1" --order 2 --jobs 4 --validate --user examples\subroutines\umat_elastic.f` | order 1–2 Validated |
| B2 | Opaque **precompiled** UMAT through the CLI | `& $cli usersub examples\subroutines\umat_elastic.f --out umatlib`, then repeat B1 with `--user umatlib` | identical results to B1; ABSS never reads the source |
| B3 | UEL with `*UEL PROPERTY`, with validation | `& $cli run examples\models\uel_props.inp --user examples\subroutines\uel_truss.f --order 1 --validate --params uel:UTRUSS:props:1,uel:UTRUSS:props:2` | `du/dPROPS1 = −2.5e-7`, `du/dPROPS2 = −5e-3` (analytic part passed in A6); Validated against reruns |
| B4 | UEL JPROPS | same deck | JPROPS shown as not differentiable (info only) |

ABSS builds the library once and points every nominal, probe, commit and rerun job at it through `usub_lib_dir`
(a job-directory `abaqus_v6.env`).

## C. Pending: mechanisms that are implemented or refused, but not yet validated

| ID | Model to build | Purpose |
|---|---|---|
| C1 | CPS8 + CPS6 mixed (`examples\models\cps8_mixed.inp`) | stress recovery at quadratic-element integration points; order 1–2 vs reruns |
| C2 | C3D8 block with spatially varying DLOAD (`examples\models\c3d8_block.inp`, flattened) | 3D, DLOAD replay self-check, geometry parameter |
| C3 | S4R plate (`examples\models\s4r_plate.inp`) | rotational DOFs, section points (H-UR hypothesis) |
| C4 | NLGEOM hyperelastic + follower pressure | follower-load replay (`*DLOAD, FOLLOWER`) |
| C5 | *EQUATION / *TIE / *COUPLING models | currently refused; validating the RF-on-independent-DOF hypothesis (Noether §2.3) would allow enabling them |
| C6 | Contact with a stable active set | currently refused |
| C7 | Third-order derivatives on smooth Abaqus models | currently Experimental; needs h-sweeps showing a plateau |
| C8 | J2 CPS4 at order 2: `& $cli run examples\models\j2_plastic.inp --params "mat:STEEL:plastic:sigma_y:1" --order 2 --jobs 6 --validate` | second order through plastic increments with GMRES; not yet run (about 3 h) |
| C9 | h-shrink sweep on Abaqus: repeat A4 with `--rel-step 1e-4`, then `1e-6` | spec §7 "as h shrinks"; not yet run |

Each C test: run `abss-cli run … --validate` and record the commands and the per-order maximum relative errors in
VALIDATION_RESULTS.md.
