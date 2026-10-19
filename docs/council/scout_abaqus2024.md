# Scout: Abaqus 2024 capability verification for ABSS

Owner: Scout. Date: 2026-10-16. Mode: verification. The affective layer was enabled but did not influence any conclusion.
Environment: Abaqus 2024 (`abaqus information=release` reports `2023_09_21-07.55.25 RELr426 190762`), Windows 11, licensed via FlexNet, `cpus=1`.
Evidence directory: `validation/experiments/scout/` (scripts, `.inp` and `.json` are tracked. `.fil/.mtx/.dat/.odb` are git-ignored but kept locally for the base jobs).
No local HTML documentation is installed. The web documentation for 2025 is at docs.software.vt.edu. Its 2024 pages returned HTTP 403 to automated fetch. Every 2024 claim below rests on a **run on the installed 2024 build** unless it is marked *doc-only*.

## Bottom line

1. **Use `*ELEMENT MATRIX OUTPUT` to get a per-increment K in a single run. It works in a general nonlinear `*STATIC` step in 2024.** It writes element stiffness at the **converged state of every increment**, including NLGEOM geometric stiffness, at about 14 significant digits. ABSS assembles the matrices itself, and no BCs are applied. The output matched FD of the reaction-force oracle to 1e-12 (linear) and 1e-7 (NLGEOM, ε=1e-7, O(ε) truncation). It matched an independent Gauss-quadrature CPS4 K to 1e-14.
2. **This K, and `*MATRIX GENERATE` after a restart, is NOT the consistent elastoplastic tangent when built-in Mises plasticity is actively yielding.** In that case both give the elastic-modulus stiffness at the current configuration. The finding was reproduced on two element types. Truss at a plastic increment: EMO and MG give K33 = 1860.19, while the FD loading tangent is 1233.52 (51% difference). CPS4 patch at a plastic increment: forward FD with ε≥1e-5 differs from EMO by 76%, while backward (unloading) FD matches EMO to 2.5e-5. This agrees with the documented rule for perturbation steps ("initial elastic modulus for elastic-plastic materials"; 2017 doc text, see sources). Whatever drives the sensitivity solve on plastic increments must not assume the Abaqus K is the algorithmic tangent.
3. **`*OPERATOR OUTPUT` is accepted by 2024 but cannot be read with supported 2024 tools.** It writes `jobname_STATIC1.sim` holding one `GenericSystem/userMatrices` per increment. `abaqus mtxasm` 2024 rejects that file with `no assembled operators found ... Incorrect SIM document?`. **`abaqus mtxasm` itself exists and works on the SIM from `*MATRIX GENERATE`.**
4. **Use `output_precision=full` for double precision.** This command-line option makes ODB U, RF, S and PEEQ `DOUBLE_PRECISION`, matching `.fil` doubles to ≤5.1e-16 relative. The default ODB is float32 (≤5.1e-8). The ASCII results file (`*FILE FORMAT, ASCII` + `*NODE FILE`/`*EL FILE`) gives 16-digit doubles. `.dat` prints 8 digits.
5. **Restart at an arbitrary increment works, and replay is bit-exact.** Syntax: `*RESTART, WRITE, FREQUENCY=1` in the base job, then `abaqus job=new oldjob=base` with `*RESTART, READ, STEP=s, INC=i, END STEP` and a new step using `*BOUNDARY, OP=NEW` that prescribes all DOFs. Replaying increment 7 from a restart at increment 6 reproduced U, RF, S and PEEQ to the last bit. **Material changes on restart are silently ignored.** The job completes with only a `***WARNING: THIS MODEL DEFINITION KEYWORD IS IGNORED IN A RESTART ANALYSIS`.
6. **Abaqus Python is 3.10.5, and `from odbAccess import openOdb` works.** The import takes 0.14 s. Reading 11 frames of U/RF/S from a small ODB took 0.02 s.

## Capability table

| Feature | Works in 2024? | Evidence (validation/experiments/scout) | Exact syntax that worked | Caveats |
|---|---|---|---|---|
| `*OPERATOR OUTPUT` in general `*STATIC` | Parsed and run; **output unreadable** | `t1_opout*`, `t1_ebe*`, `t1_opout2*`, `t3_base_STATIC1.sim`; `probe5_opout.txt`, `probe8.py` | `*OPERATOR OUTPUT, OPERATOR=ASSEMBLED, STIFFNESS, FREQUENCY=1` (also tried `ELEMENT BY ELEMENT`; `LOAD, SOLUTION, BOUNDARY CONDITIONS=NO`) | The SIM has `model/systems[n]/userMatrices` (DofDofGrid), one per increment. 2024 `mtxasm` reports `=== Error: no assembled operators found in t1_opout_STATIC1.sim.  Incorrect SIM document? ===`. With `oldjob=`, it reports `no element operators found`. The undocumented `csimAccess` Python API can navigate the tree, but sparse reads need a SparseIndices type that Python does not expose. Treat it as unusable in 2024. |
| `abaqus mtxasm` | **Yes** (exists) | `abaqus_help.txt`; `t2_mg_X2_STIF-1.mtx` | `abaqus mtxasm job=t2_mg_X2 text` → `t2_mg_X2_STIF-1.mtx` | Works on the `*MATRIX GENERATE` SIM (`jobname_Xn.sim`) but not on the `*OPERATOR OUTPUT` SIM. Its output equals the `*MATRIX OUTPUT` file plus a header line. Native usage: `-job <name> [-oldjob <name>] [-text]`. |
| `*MATRIX GENERATE, STIFFNESS` after a general step | **Yes** | `t2_mg*`, `t3_mg_i4/i7`, `c4_*_mg_i*`; `exp_truss_restart_i*.json`, `exp_cps4_*.json` | `*STEP` / `*MATRIX GENERATE, STIFFNESS` / `*MATRIX OUTPUT, STIFFNESS, FORMAT=MATRIX INPUT` / `*END STEP` → `job_STIF<step>.mtx` | Linear perturbation: K is taken at the base state, which is the end of the previous general step. For an arbitrary increment, restart with `END STEP` first (one job per increment). BCs appear as a **1e36 diagonal penalty** with off-diagonals kept. It equals EMO to ≤7.6e-15 off the penalty diagonal. **Elastic modulus under plasticity.** |
| `*ELEMENT MATRIX OUTPUT` in general `*STATIC` | **Yes**, every increment | `t4_emo.mtx`, `c4_*_base_emo.mtx`; `cmp_emo.py`, `exp_cps4_*.json` | `*ELEMENT MATRIX OUTPUT, ELSET=ALLE, STIFFNESS=YES, OUTPUT FILE=USER DEFINED, FILE NAME=<name>, FREQUENCY=1` (history data inside the step) | Per element: header `** ELEMENT NUMBER e STEP NUMBER s INCREMENT NUMBER i`, node list, active-DOF list, then `*MATRIX,TYPE=STIFFNESS` lower triangle in node-major DOF order. **No BCs.** Taken at the converged state. Includes NLGEOM geometric stiffness. **Elastic modulus under plasticity.** Untested: MPC/tie/contact/load stiffness, UEL/UMAT, instance-qualified labels. Text size grows as elements × increments. |
| Restart at increment i plus a new all-DOF-prescribed step | **Yes** | `t3_p*`, `c4_*_p*`, `t7_replay.json` | Base: `*RESTART, WRITE, FREQUENCY=1` (model data). Probe: `*RESTART, READ, STEP=1, INC=i, END STEP` / `*STEP, NLGEOM=YES` / `*STATIC, DIRECT` / `1.0, 1.0` / `*BOUNDARY, OP=NEW` / `node, dof, dof, value` lines / `*NODE FILE` / `U, RF` / `*END STEP`. Command: `abaqus job=probe oldjob=base interactive ask_delete=OFF cpus=1` | Loads carry over at their **value at increment i**. To replay i+1, give `*CLOAD, OP=NEW` with the t_{i+1} values and a step time of Δt. The probe job inherits restart writing (about 3 MB per job here). Suppress it with `*RESTART, WRITE, FREQUENCY=0` placed **after the `*STATIC` data line**. Placed before `*STATIC` it is a fatal error (`A PROCEDURE OPTION IS MISSING FOR STEP`). Wall time is about 13 s per probe job, dominated by startup. |
| Replay fidelity | **Bit-exact** | `t7_replay.json` | Restart at INC=6, prescribe u7* (from `.fil`), CLOAD at 0.7·F, `*STATIC, DIRECT 0.1, 0.1` | RF, S and PEEQ are identical to the base increment 7. The free-DOF RF equals the converged equilibrium residual (about 1e-2 on loads of order 1e2 here). It is deterministic and cancels in differences. |
| Change material on restart | **No** (silent) | `t6_matchange.dat` | Putting `*MATERIAL`/`*ELASTIC`/`*PLASTIC` in the restart deck | `***WARNING: THIS MODEL DEFINITION KEYWORD IS IGNORED IN A RESTART ANALYSIS`. The job COMPLETES. ABSS must scan `.dat` for this warning and must start parameter branches from t=0. |
| Double-precision ODB | **Yes** | `odb_precision.py` output; `t5_full.odb` vs `t5_single.odb` | `abaqus job=X ... output_precision=full` | U, RF, S, PEEQ = `DOUBLE_PRECISION`, matching `.fil` to ≤5.1e-16. SDV is untested (no UMAT/DEPVAR model). The default is `SINGLE_PRECISION` (≤5.1e-8 relative). Read with `value.dataDouble`. |
| ASCII results file | **Yes** | `t3_base.fil`, `scout_lib.read_fil` | `*FILE FORMAT, ASCII` (model data, before the step) + `*NODE FILE, FREQUENCY=1` `U, RF` + `*EL FILE, FREQUENCY=1` `S, PE, LE` | Records are 80-column wrapped. `I` + 2-digit width + digits; `D` + 22 chars (16 significant digits); `A` + 8 chars. Keys: 2000 = increment header (total time, step time, …, step, inc), 1 = element/IP header, 11 = S, 22 = PE (components, yield flag, PEEQ last), 89 = LE, 101 = U, 104 = RF. `PEEQ` is **not** accepted by `*EL FILE` (`***ERROR: OUTPUT REQUEST PEEQ IS NOT AVAILABLE FOR THIS OPTION`). Use `PE`. |
| `.dat` print | Yes, 8 digits | `t3_base.dat` | `*NODE PRINT, FREQUENCY=1` `U, RF` | Insufficient for FD. |
| `abaqus job=... double` | Exists (`double={explicit\|both\|off\|constraint}`) | `abaqus_help.txt` | n/a | This is solver precision for Explicit/constraints. It is not an output-precision option. |
| ODB API | **Yes** | `odb_precision.py` | `from odbAccess import openOdb; odb = openOdb(path, readOnly=True)` | Python 3.10.5. **ODB sequences do not support slicing** (`OdbError: Slice for an ODB Sequence is not supported.`). Iterate and filter instead. |

## Numerical validation summary

All FD values are forward differences of RF from restarted jobs with every DOF prescribed, `K[:,j] ≈ (RF(u*+ε e_j) − RF(u*))/ε`. u* comes from the double-precision `.fil`.

| Model / state | Compared | Max relative difference |
|---|---|---|
| T2D2 two-bar, linear, NLGEOM=NO | MG vs analytic EA/L (2000/√2 = 1414.2136; 707.107 couplings) | exact to printed digits |
| T2D2, NLGEOM, elastic increment 4 | MG vs EMO vs FD(ε=1e-7) | MG≡EMO; vs FD 1.0e-7 (FD converges O(ε) across 1e-4…1e-7) |
| T2D2, NLGEOM, **plastic** increment 7 | MG/EMO K_33 = [[1860.19, −227.73], [−227.73, 1387.18]] vs FD [[1233.52, 355.76], [355.76, 843.90]] | **0.51**. FD is symmetric to 1e-7, and PEEQ grows in the probes. |
| CPS4 2×2, linear (E=1000, ν=0.3) | EMO vs independent 2×2-Gauss plane-stress K; EMO vs FD; MG vs EMO | 1.1e-14; 8.5e-13; 7.6e-15 |
| CPS4, NLGEOM elastic (≈6% strain) | EMO vs FD(1e-7); MG vs EMO | 1.06e-7; 6.0e-15 |
| CPS4, NLGEOM **plastic** increment 5 (all IPs yielding) | EMO vs FD(1e-7), all 18 columns | 1.2e-7, **but PEEQ did not change in any ε≤1e-6 probe** |
| same, stretch direction | FD fwd ε=1e-5/1e-4/1e-3 vs EMO·v; bwd | fwd 0.76 (PEEQ grows ≈1.5ε); bwd 2.5ε |

Interpretation:
- **Observed:** EMO and MG give the unloading (elastic-modulus) stiffness on plastic increments.
- **Observed:** For CPS4/Mises, forward probes with ε≲1e-6 (strain about 2e-6) did not trigger plastic flow, while ε≥1e-5 did. For T2D2, ε=1e-7 already triggered flow. The mechanism (a yield tolerance or the integrator) is **unknown**.
- **Inference:** FD step sizes for residual or pseudoload probes on plastic models must exceed this activation threshold. Otherwise they silently measure the elastic branch. The threshold depends on the element and material.

## Recommended pathway (for Gauss/Ada; inference, not yet validated end-to-end)

- **Nominal run:** `output_precision=full`, `*RESTART, WRITE, FREQUENCY=1`, `*ELEMENT MATRIX OUTPUT ... FREQUENCY=1`, plus an ASCII `.fil` for U/RF/S/PE as a cross-check. Read the double-precision ODB with `odbAccess` (`dataDouble`).
- **K_i:** assemble the EMO blocks and apply the BC partition in ABSS. These blocks are verified for elastic and NLGEOM-elastic cases. On increments with active plasticity they are the elastic K. Either flag them, or use them as a preconditioner for a matrix-free Krylov solve whose K·v comes from restart-probe FD (one job of about 13 s per matvec on small models). Check the FD step against the activation threshold. Unverified for UMAT (likely DDSDDE-based; untested, because no Fortran compiler was found on PATH), contact, MPC, follower loads and hyperelasticity.
- **Probes and replays:** restart from the base or perturbed branch at increment i−1 with `END STEP`, run a new step with `*BOUNDARY, OP=NEW` prescribing all DOFs, `*CLOAD/*DLOAD, OP=NEW` at the t_i values, step time Δt_i, `DIRECT` single increment, and `*RESTART, WRITE, FREQUENCY=0`. Verified bit-exact against the base increment.
- **Parameter changes:** apply them only in a fresh full run from t=0. Restart ignores material edits with only a warning.

## Reproduction (PowerShell, from `validation/experiments/scout`)

```powershell
$A = "C:\SIMULIA\Commands\abaqus.bat"
cmd /c "$A job=t1_opout interactive ask_delete=OFF cpus=1"; cmd /c "$A mtxasm job=t1_opout_STATIC1 text"   # fails (OPERATOR OUTPUT)
cmd /c "$A job=t2_mg interactive ask_delete=OFF cpus=1";    cmd /c "$A mtxasm job=t2_mg_X2 text"           # works
cmd /c "$A job=t3_base interactive ask_delete=OFF cpus=1"     # truss NLGEOM+plastic base with restart
cmd /c "$A python exp_truss_restart.py 4"; cmd /c "$A python exp_truss_restart.py 7"
cmd /c "$A job=t4_emo interactive ask_delete=OFF cpus=1"; cmd /c "$A python cmp_emo.py"
cmd /c "$A python exp_cps4.py lin 3 1e-6"; cmd /c "$A python exp_cps4.py nl 5 1e-7"; cmd /c "$A python exp_cps4.py pl 5 1e-7"
cmd /c "$A python exp_cps4_pm.py 5 1e-6"; foreach ($e in "1e-6","1e-5","1e-4","1e-3") { cmd /c "$A python exp_cps4_pm2.py 5 $e" }
cmd /c "$A job=t5_full interactive ask_delete=OFF cpus=1 output_precision=full"; cmd /c "$A job=t5_single interactive ask_delete=OFF cpus=1"; cmd /c "$A python odb_precision.py"
cmd /c "$A job=t6_matchange oldjob=t3_base interactive ask_delete=OFF cpus=1"   # inspect .dat warnings
cmd /c "$A python exp_replay.py"
```
`exp_cps4.py` creates and runs `c4_<case>_base` itself if its `.fil` is absent. Each CPS4 case takes about 20 jobs and about 4.5 min.

## Sources (citation matrix)

| Source | Version / retrieved | Claim supported | Limits |
|---|---|---|---|
| *OPERATOR OUTPUT keyword reference, docs.software.vt.edu/abaqusv2025/English/SIMACAEKEYRefMap/simakey-r-operatoroutput.htm | 2025 doc, 2026-10-16 | Parameters, including `BOUNDARY CONDITIONS` (default YES = penalty), STIFFNESS "at convergence", static/heat transfer only | 2025 text. The 2024 page (simakey-r-operatoroutput.htm under abaqusv2024) appeared in search results but returned 403 to fetch. |
| Generating matrices from nonlinear procedures, …/abaqusv2025/…/simaanl-c-mtxgenerationgeneral.htm | 2025, 2026-10-16 | `jobname_STATICn.sim`; only upper-triangular nonzeros for symmetric; points to the assembly utility | Does not describe the text format or the 2024 reader status |
| Element matrix assembly utility, …/abaqusv2025/English/SIMACAEEXCRefMap/simaexc-c-mtxassemblyutil.htm | 2025, 2026-10-16 | `abaqus mtxasm job= [oldjob=] text`, `jobname_<STIF\|MASS\|…>N.mtx` | The 2024 binary rejected the OPERATOR OUTPUT SIM (experiment) |
| *ELEMENT MATRIX OUTPUT keyword reference, …/abaqusv2025/…/simakey-r-elementmatrixoutput.htm | 2025, 2026-10-16 | Parameters ELSET, STIFFNESS, OUTPUT FILE=USER DEFINED, FILE NAME, FREQUENCY; USER ELEMENT LINEAR format | Does not state the procedures or tangent type. Behavior established here by experiment. |
| Linear perturbation steps, abaqus-docs.mit.edu/2017/English/SIMACAEGSARefMap/simagsa-c-stptimelinpert.htm | **2017** doc, 2026-10-16 | "initial elastic modulus for elastic-plastic materials and the tangent modulus for hyperelastic materials" | Old version. The 2024 VT page returned 403. The 2024 behavior was confirmed experimentally for MG, and the same behavior was observed for EMO. |
| Installed `abaqus help` (`abaqus_help.txt`) and `abaqus mtxasm help` | 2024 build 2023_09_21 | Existence of `mtxasm`, `output_precision={single\|full}`, `double=` | Local primary source |

## Open questions

- Is EMO the DDSDDE-consistent K for a UMAT? No compiler was found on PATH (`ifort`/`ifx`/`cl` absent), so this is untested.
- Can `*OPERATOR OUTPUT` userMatrices be read through a supported route, such as a 2025+ `mtxasm` or `abaqus odb2sim`/`sim2…` tooling? If so, does that K equal the consistent tangent under plasticity?
- What is the plastic activation threshold vs ε for Mises in other elements (CPE4, C3D8, shells), and does a yield tolerance setting exist?
- What are the EMO/MG node labels for part-instance assemblies, and how do MPC/tie/contact/load-stiffness contributions appear (EMO is element-only, so constraints are absent)?
