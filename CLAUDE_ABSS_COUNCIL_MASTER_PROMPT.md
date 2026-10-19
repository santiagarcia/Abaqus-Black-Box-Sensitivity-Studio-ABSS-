# CLAUDE MASTER PROMPT — ABAQUS BLACK-BOX SENSITIVITY STUDIO (ABSS)

You are Claude Code working **locally** on my machine. This is an implementation assignment, not a request for a proposal or slides. Use the **local branch/worktree of my existing Council project** as the development/orchestration environment. Delegate meaningfully to its available specialist agents, reconcile their reviews, and build and test the software. Do not stop after producing an architectural plan, UI mockup, skeleton, or a collection of stubs. If parts cannot be executed without an installed Abaqus or license, implement what can be tested locally, clearly identify which Abaqus-dependent tests remain pending, and provide exact commands and artifacts for completing them.

## 0. Mission and non-negotiable outcome

Create **Abaqus Black-Box Sensitivity Studio (ABSS)**: a professional, fast, beautiful, genuinely simple **native C++ desktop application** for computing, inspecting, visualizing, comparing, and exporting parameter sensitivities of Abaqus/Standard models across **every converged time increment**. A scientist should normally be able to select **one INP file**, review auto-discovered parameters (geometry, materials, loading, boundary conditions, *USER MATERIAL/UMAT properties and *UEL PROPERTY parameters), select a derivative order and desired outputs, press **Run**, and explore the results in the **same application window** without viewing code, editing INPs, using a terminal, or knowing FE element formulations.

**Scientific differentiator:** leverage Abaqus as an *opaque residual and tangent operator oracle*. Do NOT reimplement the residual of CPS4, CPS8, C3D8, S4, S4R, or any other element family. Do NOT require the source of a UMAT or UEL. Abaqus owns element computations, integration, assembly, constitutive updates and history. Our program owns carefully designed INP transformations, parameter perturbations, job management, stiffness extraction, residual recovery, history-consistent finite differences, tangent-system solves, response sensitivity computation, validation, and 2D/3D display. A precompiled compatible JHU/IMQCAM UMAT or UEL must be supported when its input parameters are externally controllable and the binary is supplied or discoverable.

The attached/referenced `abaqus_odb_viewer.py` is the existing visual **functional reference**. Read it fully before replacing any functionality. It is a Tkinter launcher + Abaqus-Python ODB extractor + Plotly/Dash browser viewer and already supports discovery of steps/frames/fields, multi-instance meshes, numerous solid/shell/line types, section-point data, nodal and element coloring, deformation overlay, edge rendering, color range modes, color palettes, interactive camera, increment slider/playback, click-to-plot local time histories, caching, fast partial updates, demo data and friendly installation/errors. Preserve and improve this user experience in a **native Qt/VTK C++ viewer**; do not embed Plotly, Dash, Chromium, or a Python webserver as the finished interface. You may retain minimal Abaqus-side Python scripts ONLY where Abaqus's proprietary ODB/SIM APIs require them; the main app, solvers, data structures, scheduling, parsing and visualization must be C++.

## 1. Council operation: use the EXISTING LOCAL branch

1. Inspect the filesystem, Git repositories, Git branches/worktrees, current status, existing Council agents/configuration and project tooling. Find **the local Council branch** already available on this machine; do not silently substitute some remote unrelated `the-council` repository. If ambiguous, inspect existing metadata/working directories and select the most relevant local worktree without overwriting user changes. Preserve unrelated work; do not hard-reset, delete branches, alter user secrets, or overwrite existing programs.
2. Read Council's actual instructions, agent contracts and local dependencies. Use its existing mechanism for parallel specialist review and integration, not an imaginary council invocation. Suggested roles (map onto the actual roles present): Abaqus INP/ODB/SIM/restart expert; computational mechanics and nonlinear history; numerical differentiation/high-order sensitivity; sparse algebra/Kokkos/GPU; C++/Qt/VTK desktop UX; testing/verification; privacy/licensing/security; release/build engineer.
3. Have at least two independent reviews of the residual/reaction-force strategy and higher-order/history recursion. Record dissent, testable risks, decisive experiments, and resulting choices in `docs/COUNCIL_REVIEW.md`. Agents should produce working modules/tests, not just opinions.
4. Implement and integrate locally, run all tests available on the machine, address failures, report tested versus not tested without fabricating Abaqus runs. Commit coherent changes **only if** the repository's actual workflow permits it; otherwise preserve working tree and report diffs.

## 2. Verified technical starting points — but DO NOT assume they automatically prove our method

Consult local Abaqus docs and the official docs for the **installed version** before emitting keywords. Useful official documentation:
- Generating matrices from nonlinear procedures: https://docs.software.vt.edu/abaqusv2025/English/SIMACAEANLRefMap/simaanl-c-mtxgenerationgeneral.htm
- SIM/MTX assembly utility: https://docs.software.vt.edu/abaqusv2025/English/SIMACAEEXCRefMap/simaexc-c-mtxassemblyutil.htm
- Abaqus design sensitivity theory/history: https://docs.software.vt.edu/abaqusv2025/English/SIMACAEANLRefMap/simaanl-c-dsa.htm
- UEL API and element residual RHS: https://docs.software.vt.edu/abaqusv2025/English/SIMACAESUBRefMap/simasub-c-uel.htm
- Qt/VTK `QVTKOpenGLNativeWidget`: https://vtk.org/doc/nightly/html/classQVTKOpenGLNativeWidget.html
- Kokkos Kernels documentation: https://kokkos.org/kokkos-kernels/docs/

Investigate `*OPERATOR OUTPUT, OPERATOR=ASSEMBLED, STIFFNESS, FREQUENCY=1` within supported nonlinear Abaqus/Standard procedures, whether it returns the tangent at the desired converged states, and how to handle BCs and row/column DOF ordering. The official `abaqus mtxasm ... text` utility can write assembled matrices from SIM to indexed MTX files. CHECK version-specific availability and syntax, and verify numerically before using these operators.

The full-vector unconverged Newton residual is not generally available as a supported single simple ODB call. The proposed oracle is: prescribe **all independent relevant DOFs** at a desired displacement, preserving appropriate loads and state; read the compensating reaction forces. With `r = F_ext - F_int`, at held DOFs nominally `r = -RF` when all terms and sign conventions match. Treat this as a **hypothesis to validate**, not a universal identity for every constraint/UEL/contact/multiphysics scenario. Investigate interactions with MPC/tie/coupling, shell rotational DOFs, follower loads, inertia, contact, initial stress, unsymmetric formulation and other reactions. Do not interrupt a job after a single iteration unless that produces a demonstrably supported, complete residual vector; a completed frozen-DOF one-increment evaluation is preferred.

## 3. Derivation and algorithm — scientific specification

Let `r_i(u_i, u_{i-1}, q_{i-1}, a) = 0` be the Abaqus equilibrium equations at accepted increment i, with historical internal state `q`. Let `K_i = -∂r_i/∂u_i` using the structural positive-stiffness sign convention, and let `s_i = du_i/da` denote total derivative along the equilibrium solution branch. For fixed initial conditions and one scalar parameter:

`K_i s_i = ∂_a r_i + (∂_{u_(i-1)}r_i) s_(i-1) + (∂_{q_(i-1)}r_i) (dq_(i-1)/da)`.

We must NOT separately differentiate opaque constitutive internals. Instead:
1. Run a nominal analysis, preserve `u_i*`, accepted time grid, fields and `K_i` (or a separately verified tangent operator) at all accepted increments.
2. Run a **perturbed-history branch** from the beginning with `a+h`; at step i its previous displacement and material state must approximate the complete sensitivity-corrected history (including dependent boundary conditions).
3. **Residual probe at step i:** begin from the perturbed history through `i-1` but set *current* displacement to the nominal `u_i*` at the matching time. Extract residual on the original independent free DOFs. Approximate total *history-carrying* pseudoload `p_i ≈ [r_i(u_i*, perturbed prior history, a+h) - r_i(u_i*, nominal prior history, a)]/h`.
4. Solve `K_FF,i s_F,i = p_F,i - K_FC,i s_C,i` for free-DOF sensitivities; the prescribed DOF sensitivities `s_C,i` arise directly from differentiated BC values and coupling/constraint transforms. Carefully derive the correct constrained-coordinate form when DOF elimination is used. Do not blindly impose zero prescribed sensitivities for parameter-dependent boundaries.
5. For the **advance/commit branch**, prescribe `u_i* + h s_i` at current i (and the correct perturbed BCs), let Abaqus commit the history, then proceed to i+1. Keep the residual probe and history advance logically separate. Verify that using restarts, separate replay files, or other supported Abaqus methods preserves q and produces the intended states; if per-increment restarts cannot guarantee this, choose a controlled complete-history replay. Never change UMAT material definitions illegally midway through a restart: define the perturbed parameter at the beginning of the corresponding branch.
6. Solve sparse linear systems; do **not** compute a dense `inverse(K)`.

**Critical scientific distinction:** `r(u_i* + h s_i, a+h, perturbed history)` should be small, nominally `O(h²)` in a smooth problem, and acts as a **predictor consistency test**; it is NOT the frozen-current-displacement residual from which `p_i` must be extracted. Independently check `K_i v ≈ -[r_i(u_i*+εv)-r_i(u_i*)]/ε` with the prior history held fixed and appropriate BC treatment. Distinguish total vs partial derivatives clearly in the UI and stored data.

Use accurate, tested formulas for nonlinear output sensitivities: for stress, strain, SDVs, energies, reaction forces and other supported fields, evaluate output *after propagating the perturbed equilibrium-predicted displacement and history*. Do not pass off `dU/da` as `dS/da`. Compare consistent ODB field data at identical instance, node/element, integration/section point and accepted time, with physical component frames and local orientations handled carefully. Preserve Gauss/section point values in the computational data model; averaging to faces/vertices is for display and must be labeled as projection. Derived invariants such as von Mises require correct differentiation or finite-differencing of that invariant, not taking the invariant of the sensitivity tensor.

Geometry sensitivities require perturbing nodal coordinates or exposed shape parameters with consistent node/DOF identities and valid mesh, and accounting for `x(a)=X(a)+u(a)` and for the effect of perturbed external load directions and constraints. Reject topological remeshing and nonexistent node correspondence as unsupported unless explicit remapping is implemented and validated. BCs, load amplitudes, distributed/pressure/follower forces and tabular curves must be scanned and parameterized through reproducible edits, including step/time semantics.

### Higher-order derivatives

The **user chooses derivative order** in the UI: order 1 first, order 2 next, then 3 and higher **only when implemented and verified**. Distinguish pure derivatives `d^k y/da_j^k` from mixed derivatives `∂^(k)y/(∂a_j^p ∂a_l^q)`; do not misleadingly claim general multi-index support. Design an order-agnostic coefficient/jet representation, but test every exposed order independently.

For an analytic smooth branch one possible general recursion is to insert the lower-order displacement Taylor predictor `u_i^[<k](h) = u_i* + Σ_{j=1}^{k-1}(h^j/j!) u_i^(j)` into a full perturbed-history residual evaluation, extract its coefficient of `h^k`, and solve `K_i u_i^(k) = k! [h^k] r_i(u_i^[<k](h), a+h, consistent prior history)` under the stated sign convention, with proper prescribed DOF and history treatment. This is an algorithmic lead, not license to fake high orders: derive carefully and validate using analytically soluble models and converged full FD reference derivatives. Use finite-difference stencils or polynomial extrapolation with enough independent parameter offsets and precision, check step-size convergence and cancellation errors, and expose cost estimates and conditioning diagnostics. If third/fourth-order sensitivities fail reliability thresholds, show them as experimental/unavailable rather than generating bogus arrays. Implement a strongly verified first-order flow and at least a mathematically validated second-order path; design extensibility to 3+ and enable only passing capabilities. Order-zero nominal data must always be available.

Contact transitions, bifurcations, changing active sets, hardening/yield activation, cutbacks, adaptive time-grid changes, plasticity nonsmoothness, external state files and side effects can cause nonexistence, non-smoothness or failure of higher derivatives. Detect/report problems rather than smoothing them away. Start with quasi-static Abaqus/Standard structural cases; do not advertise universal arbitrary physics support. Treat transient dynamics and coupled multiphysics as explicit future backends requiring the correct effective tangent and state/inertial/history propagation.

## 4. Input and parameter discovery — ONE INP as the normal UX

Implement a robust **structure-preserving Abaqus INP parser/editor** (not global search-and-replace and not just regular expressions). Parse case-insensitive keyword cards, continuations, comments, numeric tables, set identifiers, step-local model definitions, `*INCLUDE` recursion, `*PARAMETER` and expressions where supported, `*NODE`, `*ELEMENT`, `*MATERIAL`, elastic/plastic/viscoelastic/hyperelastic models, `*USER MATERIAL` (`PROPS`), `*USER ELEMENT`, `*UEL PROPERTY` (`PROPS`/integer properties when applicable), `*BOUNDARY`, `*CLOAD`, `*DLOAD`, `*DSLOAD`, amplitudes, sections and properties. Preserve comments, order, formatting, include provenance, and untouched user content, with deterministic minimal edits, parsing warnings and reversible diffs. Do not conflate keyword blocks if the data span lines or includes.

Expose parameter candidates in a searchable friendly table with: human-readable name, type (geometry/material/UMAT/UEL/load/BC/section/amplitude), nominal value, units if supplied/known (Abaqus is not inherently unit-aware), source file + line or element/instance/node-set association, applicable steps, allowed perturbation bounds, inferred positivity restrictions, scalar/multi-component nature, and confidence (known semantic vs opaque indexed constant). Examples:
- `E` and `nu` for defined elastic material, physical property and parameter-table entries.
- `UMAT: MaterialName: PROPS[1] = 210000`, **not** a fabricated physical name for a compiled UMAT with unknown semantics.
- `UEL: ELSET: PROPS[3]` and user element `JPROPS` when applicable.
- Boundary U2 at an identified node/nset and step; prescribed displacement amplitude value.
- Applied force/pressure magnitude and its amplitudes.
- Node XYZ or explicitly user-defined geometric parameter controlling multiple nodes, with shape groups where discoverable.

When a model needs an external binary/auxiliary input (UMAT, UEL, include, solution-dependent file, user material property table, etc.), transparently search safe local sibling/configured paths; show a friendly **one-time missing dependency dialog** if not found. An INP alone cannot magically execute a compiled UMAT/UEL or reveal what opaque binary `PROPS[k]` means, and the UI must not pretend otherwise. Never upload proprietary code, binaries, inputs, ODBs or contents to network services. Local processing only. Avoid modifying the original input. Expose expert overrides to add explicit custom scalar controls pointing to exact file locations or parameterized include expressions. Provide a sensitivity-cost preview before running.

## 5. Native desktop UX: effortless, visually polished, professional

Build a cohesive single-window Qt 6 app. Aim for ParaView-level responsive 3D visualization with the simplicity of a well-designed scientific plotting app. Not a web dashboard. The primary workflow should be intuitive to someone who has never heard of Jacobians:

**Welcome / open**: drag-and-drop INP or Browse, recent projects, system readiness indicators for Abaqus executable/license, detected CPU/GPU/Kokkos backend, and a one-click bundled demo requiring no Abaqus.

**Import/inspect**: render mesh quickly, show parts/instances/materials/element types, steps, possible parameters grouped by category; mark missing sources, unsupported steps, unknown/uneditable parameters distinctly. Search/filter, editable parameter names, checkbox multi-selection, parameter bounds and auto perturbations with advanced controls hidden by default. Show friendly inline explanations.

**Configure**: derivative order selector (0,1,2,3,...; disabled if scientifically unsupported), pure/mixed selection if available, choose outputs U, RF, S, E/LE/PE, PEEQ, SDVs, energies, user outputs, derived magnitudes and invariants, time frame coverage (default every accepted increment), chosen units, numerical scheme, accuracy tolerance, CPU/GPU settings, Abaqus concurrent-job/licensing limitations, scratch folder and max disk usage. Provide estimated Abaqus jobs / computation time risk *without promising exact durations*.

**Run**: large clear Run/Pause/Cancel controls, progress per nominal job/parameter/order/frame, live solver/validation status, current increment, sparse-solver status, error handling with actionable non-code messages and a collapsible advanced log. Resume interrupted jobs from a manifest; honor license availability; do not terminate unrelated Abaqus processes. Run in background threads/processes so UI is always responsive, with safe cancellation and cleanup.

**Explore results (SAME WINDOW)**: main fast GPU-accelerated 3D model view and dockable left parameter/field tree, right controls, bottom linked history charts. Support 2D and 3D meshes, high-order solids/shells, multiple instances, color-filled contour plots, per-component scalar selection, iso-surfaces/clipping planes/cross sections when meaningful, orbit/pan/zoom, perspective/orthographic, fit/isometric/XY/XZ/YZ, visible mesh edges, wireframe, transparency, selection sets, undeformed/deformed overlay and deformation scale. Shell thickness / section points and directors where feasible, axis/unit metadata, missing-data opacity, legend with units and selected derivative. Efficient VTK data pipelines and progressive/out-of-core loading for large models; do not silently average data from distinct integration/section points.

**Colormap and derivative navigation**: make it effortless to choose a parameter, **derivative order**, output variable (`U`, `S11`, `S12`, Mises, `SDV[k]`, `RF`, etc), spatial location (node/integration point/section point), increment, and color map. Palettes such as Viridis, Turbo, Plasma, Inferno, Coolwarm, seismic, grayscale, plus user-defined palettes; diverging centered at zero for signed derivatives; linear/log/symmetric-log scaling when valid; globally constant vs current-frame vs manually fixed range; clear `∂U2/∂E`, `d²S_Mises/dE²`, etc labels. Optional separate display of **time derivative of a sensitivity** `d/dt(du/da)` and **spatial gradient of sensitivity** `∇(du/da)`, clearly marked as *postprocessing of spatial/time data*, not confused with higher parameter derivative. Time and space derivatives require correct sampling/interpolation and proper discontinuity labels. Do not call the mathematical derivative of the color lookup table a physical field derivative.

**History and comparisons**: interactive timeline with next/previous, play/pause/speed; display all accepted increments (do not subsample by default for computations); click a node/element/integration/section point to display nominal and first/second/higher sensitivity vs step time and accumulated total time; select multiple points for overlaid histories; hover values; plot with uncertainties/FD convergence and optional baseline vs perturbed response; synchronized multiple viewports, comparison of two parameters/orders and exportable publication-quality figures.

**Export/project**: project file captures original INP reference, hashes, resolved dependencies, parameters, source locations, perturbations, version, solver backend, all time/step/increment metadata, DOF ordering, response arrays, field location, units, convergence tests, logs and evidence. Export CSV, VTK/VTM/VTU/VTP where appropriate, HDF5 or compressed chunked binary, PNG/SVG charts, screenshots/video animation, and an optional concise HTML/PDF report if practical. A user should be able to reopen and browse results without Abaqus being installed; Abaqus needed only for generating new FE results.

## 6. Engineering architecture: C++20, Qt6, VTK, Kokkos

Use CMake + modern C++20 with modular packages, typed interfaces, explicit ownership and tests:

- `abss-core`: models, typed parameters, frame IDs, element/node/section-point identities, units, project persistence, quality flags, stable API.
- `abss-inp`: lossless-ish keyword model, safe include graph, parameters discovery, patch/diff/rewriter, line provenance, parameter value validation.
- `abss-abaqus`: executable detection, safe subprocesses, version probing, job generation, job manifests, license-aware scheduler, Abaqus-side Python for ODB/keyword API **only when necessary**, SIM operator and MTX conversion, restart/replay management, console log interpretation.
- `abss-oracle`: frozen-current-displacement residual evaluation, independent DOF map and multipoint constraint handling, reaction extraction, residual sign/correction tests, supported-case capability matrix.
- `abss-sensitivity`: first-order incremental history propagation, second-order verified recursion, higher-order jet/stencil expansion, material/geometry/load/BC perturbations, output response FD, validation and metadata.
- `abss-linalg`: sparse coordinate/CSR matrix ingestion, BC elimination/transformations, DOF permutation and matching, reusable factorization/preconditioning, nonsymmetric solver, residual checks, multi-right-hand-side strategies; no explicit inverse.
- `abss-visualization`: native VTK + Qt rendering, high-fidelity mixed-element meshes, data location conversion, colormaps, animation, point picking, history plots, large data caching, camera and cross-section tools.
- `abss-gui`: main window, wizard-style workflow, state/recent projects, project browser, controls, persistent layouts, threaded scheduling, user-facing diagnostics.
- `abss-tests` + `abss-benchmarks`: unit/integration/regression/accuracy/performance, headless CI, test INPs, synthetic ODB/MTX analogs.

**Kokkos:** parallelize large numeric arrays, batched perturbation postprocessing, sparse algebra, multi-RHS operations and CPU/GPU workloads through Kokkos/Kokkos Kernels with optional CUDA, HIP, OpenMP backends according to installed toolchains. For sparse K solves, benchmark suitable methods including Kokkos Kernels iterative routines where supported and an optional performant direct-solver integration (e.g., SuiteSparse/KLU/CHOLMOD where mathematically applicable, oneMKL/PARDISO, Trilinos/Belos/Amesos2 or an available licensed solver); direct sparse factorization and Kokkos are not interchangeable. Support symmetric positive definite **and** nonsymmetric/indefinite matrices where necessary; matrix structure and physics determine the solver. Reuse symbolic/numeric factorizations/preconditioners across dozens of parameter RHS vectors at each increment. Provide a dependable CPU fallback if no GPU or Kokkos GPU backend exists. Ensure compute acceleration yields **measured** speedup, not a badge.

**Bounded parallelism:** Abaqus jobs are expensive/license-limited and serial within a parameter's history; independent parameter branches and postprocessing may be parallelized subject to resource scheduling. High derivative order may require combinatorially more evaluations; explain cost and employ batched RHS, caching and stable stencils. Isolate scratch dirs and provide crash recovery. Keep Windows first-class and Linux/HPC buildable. If HPC ARC/Slurm is discovered, add an optional scheduler adapter without making it a prerequisite. Safely support shell script/batch differences.

## 7. Scientific correctness, validation and release gates

Do not conflate a pretty plot with scientific correctness. Produce a verification plan and **actually run what is feasible**. Prioritize these cases and report independent errors quantitatively:

1. **Analytic linear spring / 2-node truss / single CPS4 elastic** with analytic `du/dE`, `du/dF`, `d²u/dE²` and a parameter-dependent prescribed BC; demonstrate residual sign, DOF mapping, `K_FF` and nonzero BC derivative. For elementary `u=FL/(EA)`, e.g., `du/dE=-FL/(E² A)` and `d²u/dE²=2FL/(E³A)` for constant other inputs.
2. CPS8 and mixed CPS4/CPS8 stress recovery, numerical convergence; compare **native Abaqus element residual** to reaction-oracle recoveries. Test clean nodal and integration-point indexing.
3. C3D8 3D with spatially varying loading and geometry coordinate derivative; compare against complete converged reruns.
4. S4 and S4R shell membrane and bending, rotational DOF sensitivity and shell section points; carefully test nominal vs sensitivity output (including moments and rotations).
5. Nonlinear geometry hyperelastic test, including stiffening and follower-load variants.
6. Multi-increment J2 plasticity with loading/unloading and history: compare entire `s_i(t)` to full nonlinear centered FD of two complete Abaqus simulations at `a±h`, perturbation-size convergence, predictor residual `O(h²)` when smooth. Check incremental material state propagation; do not validate solely on final frame.
7. UMAT both source-available and **opaque precompiled binary** with `PROPS` perturbation. Verify it runs with zero source introspection. Validate that opaque `PROPS[k]` naming and ordering are correct and only exposed values can be differentiated.
8. UEL with `*UEL PROPERTY`, including where optional constraints/UEL reaction semantics differ; establish a capability gate rather than assuming RF always returns all UEL residual contributions.
9. Mixed constraints: MPC, tie, coupling, originally constrained and free DOFs, contact (smooth active-set only initially). Validate against an independent finite-difference re-solve, and explicitly flag unsupported cases.
10. Higher-order derivative tests order 2 and optionally 3+, using analytic manufactured examples and systematic step-size convergence. Pure vs mixed derivatives separate.
11. Regression tests for INP includes, material tables, continuation/data cards, multiple steps, amplitudes, multiple instances, large meshes, high-order element connectivity, shell/local coordinates, path names with spaces, permissions and locale.
12. GUI: mouse picking, history curve, replay, multiple colormaps, signed diverging range, time synchronization, camera state preservation, data quality flags, export/reopen, cancellation/restart, no network access to proprietary inputs.
13. Performance: report reproducible wall time and memory relative to a naive solver and to the older Plotly viewer where fair; track throughput by mesh size and time frames. Never claim improvement without measurement.

**Decisive gates before claiming a generic engine**:
- Operator matrix is numerically the same (within tolerance) as the frozen-history residual directional derivative. If Abaqus `*OPERATOR OUTPUT` cannot be trusted for an element or state, use a flagged finite-difference K-vector alternative or block unsupported cases.
- Frozen-displacement reaction residual equals the actual free-DOF imbalance on analytic examples, including original loads and constraint transforms.
- Perturbed restart/replay preserves nonlinear internal history and the same time schedule.
- Predicted perturbed full equilibrium residual scales as `O(h²)` for first-order in a smooth case; high-order method has correspondingly justified scaling.
- Full baseline/perturbed Abaqus re-solves agree with computed sensitivities as h shrinks, for both U and S, at **each** increment.
- No fabricated data, plots, benchmarks, or unverified compatibility claims.

If Abaqus is not installed in this environment, build a simulated Abaqus adapter that generates analytically correct residual/stiffness/fields and permits running the full GUI end-to-end in demo mode, and implement the real adapter behind the same interface. The demo must be **clearly labeled synthetic**. Provide an `ABAQUS_REQUIRED_TESTS.md` with precise INP jobs and commands to run on a licensed machine. Include at least one small ready-to-run Abaqus validation INP and generated oracle INP examples.

## 8. Execution plan: engineering, not theater

Implement in thin vertical slices with buildable milestones; keep pushing beyond mockups within the available environment:

A. Inspect Council + source viewer + machine prerequisites. Write `docs/ARCHITECTURE.md`, `docs/SCIENCE.md`, `docs/RISK_REGISTER.md`, capability table and milestones only after initial investigation. Independently validate oracle and matrix mechanism, preferably with actual Abaqus if available.

B. Build native C++ app with **actual** INP parser + parameter scanner + mesh and native VTK viewer + an analytical demo model, with all primary UI controls connected to state and plots. No inert/dummy buttons.

C. Implement nominal Abaqus runner, ODB adapter, stiffness matrix/SIM conversion, DOF map, residual probe generator and reaction extractor, real job scheduling, validation of residual/tangent on elastic CPS4.

D. Implement first-order sensitivity across *every accepted increment* including separate probe and commit history, and native field-history display. Support independent parameters in materials/UMAT/UEL/load/BC/geometry as validated, with per-category capability indicators.

E. Implement verified order 2 and extensible higher orders; field sensitivities at integration/section points; parallel multi-RHS sparse solves; GPU/CPU performance tests; more mixed-element and nonlinear tests.

F. Polish and package installers/build scripts (Windows and Linux as feasible), bundled examples, tutorial, concise README, developer and verification docs, screenshot/demo video if environment permits, automated test commands and project export. No mandatory external Python for the end user except vendor Abaqus's own Python bridge.

Do not discard features just because a milestone gets difficult. However, do not quietly label an unsupported feature complete; mark it experimental or disabled, document the exact missing validation, and finish everything else. Regularly run compilation and test targets and repair failures. Prefer clear normal-language labels over cryptic software jargon. Test usability with a non-expert scientist persona.

## 9. Exact deliverables and response

Inside the identified local worktree create or safely integrate:

- A buildable CMake/C++20 project (`src`, `include`, `apps`, `tests`, `examples`, `docs`, `third_party` or dependency manifest as appropriate).
- Working Qt6/VTK desktop binary and demo mode; functioning native mesh + sensitivities timeline + colormaps + click-history plot.
- Kokkos backend plus tested CPU fallback, sparse solver capability detection and real residual checks.
- Real Abaqus INP/ODB/SIM orchestration and sensitivity backend for scientifically verified element cases, with appropriate mandatory Abaqus-generated runtime scripts only where essential.
- Ready-to-run example INPs and tests, including a small UMAT and a UEL; compiled-opaque subroutine demonstration if the machine supports compiling it.
- A self-contained `README.md` with installation, build, demo, actual Abaqus run instructions, GUI walkthrough, all requirements, test outcomes and supported/unsupported features.
- `docs/SCIENCE.md` with exact sign conventions, differentiation equations, frozen residual tests, high-order theory and limitations.
- `docs/COUNCIL_REVIEW.md`, `docs/VALIDATION_RESULTS.md` (measured numbers and commands), and `docs/KNOWN_LIMITATIONS.md`.
- Executable test suite + test summary (`ctest` or equivalent), and packaging scripts.

At completion, report: actual local path/branch, changed files, build commands and exit codes, tests that truly ran and their errors, screenshot locations if generated, first launch command, demo walkthrough, and precisely which Abaqus/UMAT/UEL/order/features are already verified vs still experimental. DO NOT present intent as completion. If something fails, say exactly what, preserve the working implementation and explain the reproducer.

Start by inspecting the attached Python viewer and the local Council worktree; then delegate and **implement**. I do not want a slide deck, architectural speculation, a Python Plotly re-skin, or a chat-only answer. Deliver a real native, scientifically validated tool with simple UX.
