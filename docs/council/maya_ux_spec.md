# ABSS native UX specification (Maya)

Owner: Maya (human–system interaction). Status: **design specification, not usability evidence.**
Inputs read in full: `CLAUDE_ABSS_COUNCIL_MASTER_PROMPT.md` §0–§9 (§0, §4, §5 normative), `reference/abaqus_odb_viewer.py` (2520 lines).
Everything below marked "walkthrough" is a designer hypothesis until the §9 test script is run with real participants.

Conventions: **P0** = required for first release (blocks "Working Qt/VTK GUI" success criterion); **P1** = next release; **P2** = later.
`ref:L123` = line in `reference/abaqus_odb_viewer.py`.

---

## 1. Non-negotiable interaction rules (apply everywhere)

1. **No inert controls.** Every visible control either works or is disabled *with a tooltip that says why and what enables it*. A control that cannot work in this build (feature not implemented) is **hidden**, not disabled. Enable rules are in §6.
2. **The UI never blocks.** INP parsing, mesh building, Abaqus jobs, sparse solves, range computation and export run off the GUI thread (QThread / QtConcurrent). The GUI shows a progress indicator within 200 ms of any operation expected to take > 0.5 s.
3. **Synthetic data is always labeled.** Demo results show a persistent amber banner "Demo data — computed from an analytic model, not from Abaqus" in the Explore page, the window title suffix "— DEMO (synthetic)", the color-bar footer, and every export (CSV header line, PNG footer text).
4. **Capability gating is data-driven.** Which derivative orders / parameter categories / outputs are enabled comes from the backend capability matrix (`abss-core`), never from hard-coded UI booleans. The UI shows the matrix's status word verbatim: *Verified*, *Experimental*, *Not available*.
5. **No silent averaging or subsampling.** Any display projection (integration points → element average, element → node) is named in the legend footer. Computations default to every accepted increment; any display stride is shown in the timeline label.
6. **Errors are actionable.** Every error = plain-language title + one-sentence cause + "What you can do" (≤ 3 bullets) + "Show details" (expands raw log) + "Open log folder". Never blame the user ("You entered an invalid…" → "This value can't be used because…").
7. **Local only.** No network calls. No telemetry. Nothing in the UI offers upload/sharing.
8. **Plain language first, notation second.** Every derivative is shown as notation *and* a sentence (§5.3).

---

## 2. Reference viewer feature inventory → native mapping

| # | Reference capability (locator) | Native equivalent (Qt6/VTK) | Pri |
|---|---|---|---|
| 1 | One-file workflow: only the .odb needed (ref:L21) | One INP in; ABSS also opens its own project file and, P1, a bare `.odb` for viewing nominal fields only | P0 (INP, project) / P1 (ODB) |
| 2 | Abaqus auto-discovery: PATH names `abaqus`, `abq20xx`, SIMULIA/Dassault folder globs on drives C–H, Linux paths (ref:L1060-1097) | `abss-abaqus` detector with the same search list; result shown in Welcome readiness row; "Choose…" button to override; persisted in QSettings | P0 |
| 3 | Abaqus launch fallback via `cmd /c call` on Windows (ref:L1188-1190) | Same fallback in subprocess layer; invisible to user | P0 |
| 4 | "Abaqus found. Nothing to do here." / red hint with exact `abaqus.bat` location (ref:L2269-2275) | Readiness row text, §4.1 | P0 |
| 5 | Old-version ODB auto-upgrade to a copy (ref:L697-704) | When ODB reading is used (nominal/perturbed results), upgrade a copy in scratch; log message "This result file comes from an older Abaqus version. Using an upgraded copy." | P1 |
| 6 | Non-ASCII path workaround: hard-link/copy to plain name (ref:L1110-1135) | Scratch dir always ASCII, no spaces; INP + dependencies staged there; message only when a copy (not link) is needed | P0 |
| 7 | Failure explainer: locked ODB, command not started, newer-version ODB, license, out of memory, disk full (ref:L1138-1155) | `abss-abaqus` log interpreter returns `{code, title, cause, actions[]}`; UI renders per rule 6. Extend with: license queued, UMAT/UEL compile/link failure, missing include, convergence failure (with increment number), job killed | P0 |
| 8 | Error dialog offering to open details file (ref:L2249-2257) | Error panel with "Show details" + "Open log folder" (QDesktopServices) | P0 |
| 9 | Private app folder, single place to delete; setup/error logs (ref:L87-113) | `%LOCALAPPDATA%/ABSS` (QStandardPaths::AppLocalDataLocation): `cache/`, `logs/`, `scratch/` (scratch overridable). Shown in Settings ▸ Storage with "Open folder" | P0 |
| 10 | First-run pip install (ref:L186-285) | **Dropped by design**: native installer bundles Qt/VTK; no internet needed. Only Abaqus's own Python is used | P0 (absence) |
| 11 | Mesh: multi-instance, node/element label LUTs with gaps (ref:L759-835, L846-867) | `abss-core` stable IDs (instance, label); picking reports Abaqus labels, never internal indices | P0 |
| 12 | Element classification solid/surface/line/skip from type prefix + node count; unknown names decided by node count (ref:L1247-1287) | Same table in `abss-visualization`; ported literally, then extended (high-order nodes kept for P1 curved rendering) | P0 |
| 13 | Exterior surface extraction: faces owned by one element only; quads split to triangles; unique feature edges (ref:L1290-1357) | `vtkUnstructuredGrid` with native VTK cell types (VTK_HEXAHEDRON, VTK_QUADRATIC_HEXAHEDRON, VTK_WEDGE, VTK_TETRA, VTK_QUAD, VTK_TRIANGLE, VTK_LINE…) → `vtkDataSetSurfaceFilter` (nonlinear subdivision 1 for quadratic). No manual triangle split needed | P0 |
| 14 | Element-type summary table "Type / Count / Kind" with instance tooltip (ref:L1864-1866, L1286) | Model panel tree: Instance ▸ element type "C3D8 × 864 — 3D solid". Kind text uses the reference wording: "3D solid", "shell / 2D", "beam / truss / connector", "not drawn" | P0 |
| 15 | Line elements drawn as thick colored lines (ref:L1809-1817) | Line cells rendered with `RenderLinesAsTubes` (width 4 px) | P0 |
| 16 | Points/1-node elements skipped (ref:L1268) | Same; listed as "not drawn" in model panel | P0 |
| 17 | Field discovery: all steps' first+last frames, components, invariants, positions, descriptions (ref:L727-756) | Output catalogue from nominal ODB; shown in Output selector with description tooltip | P0 |
| 18 | Variable list: Magnitude for vectors, Mises for tensors, each component, hide components never defined (ref:L1421-1433) | Same rules in the Output combo; Mises entry only when MISES invariant valid | P0 |
| 19 | Default variable "S: Mises" else "U: Magnitude" (ref:L1435-1440) | Nominal default same; for sensitivities default = first selected parameter, order 1, U: Magnitude → see §5.2 | P0 |
| 20 | Duplicate names disambiguated "(nodes)" / "(elements)" (ref:L1411-1414) | Location shown explicitly in the "Where" combo instead | P0 |
| 21 | Section points kept separate: "S [section point 5]" (ref:L972-974) | "Where" combo: "Section point 5 (top)" when description known; never averaged across section points | P0 |
| 22 | **Integration points averaged per element without label** (ref:L936-956, bincount mean) | **Change:** element-average is a labeled display projection: legend footer "Shown: element average of 8 integration points (display only)". P1: choose individual integration point. Computation keeps IP values (§5 of master prompt) | P0 (label) / P1 (per-IP) |
| 23 | Missing-data faces/lines in grey "no data" (ref:L1479-1497, L1800-1817) | Cells with NaN rendered with `vtkLookupTable::SetNanColor` = token `nodata`; legend shows a grey swatch "No data here" | P0 |
| 24 | Mises computed from components if not supplied (ref:L870-878) | For **nominal** display only. For sensitivities, d(Mises)/da must come from the backend (master prompt §3); UI never derives it from component sensitivities | P0 |
| 25 | Deformed shape toggle, disabled if no U (ref:L1876-1877) | "Show deformed shape" checkbox; disabled tooltip "This result has no displacement (U) output." | P0 |
| 26 | Deformation scale numeric input (ref:L1884-1886) | `QDoubleSpinBox` 0–1e6, default 1.0, plus "Auto" button (largest displacement = 10 % of model size) | P0 (spin) / P1 (Auto) |
| 27 | Mesh edges toggle; off by default above 150 000 edges (ref:L1867) | Same default rule; `vtkProperty::EdgeVisibility` | P0 |
| 28 | Camera kept across data updates (ref:L1651-1721) | VTK keeps camera unless Fit/view button; `ResetCamera` only on first load and Fit | P0 |
| 29 | Fixed bounding box = envelope of deformed shape over all frames (ref:L1539-1555) | Compute once per (scale) off-thread; used by Fit so animation never jumps | P1 |
| 30 | View buttons Fit / Iso / XY / XZ / YZ (ref:L1879-1883) | Toolbar on the 3D view, same five + "Perspective/Orthographic" toggle | P0 |
| 31 | 2D auto-detect → orthographic top view, Z axis hidden (ref:L1419, L1833-1836) | Same; rotation locked to in-plane by default ("Lock 2D view" checkbox, P1) | P0 |
| 32 | Orbit drag, scroll zoom, right-drag pan; help text (ref:L1900-1902) | `vtkInteractorStyleTrackballCamera`; middle-drag also pans; same help text in the empty history panel | P0 |
| 33 | Colormaps Turbo, Jet, Rainbow, Viridis, Plasma, Cividis, RdBu_r, Greys; default Turbo (ref:L1770) | §8.5 list; **defaults change** to Viridis (unsigned) / Cool–warm centered (signed) | P0 |
| 34 | Color limits: all frames / current frame / manual min-max (ref:L1889-1897, L1511-1526) | Same three + "Symmetric about zero" checkbox; manual fields **disabled unless Manual** (reference left them always editable — inert) | P0 |
| 35 | Degenerate range padding (hi ≤ lo) (ref:L1523-1525) | Same; legend shows "All values = 3.2e-5 (constant)" | P0 |
| 36 | Color bar with title, 4 significant digits (ref:L1843-1845) | `vtkScalarBarActor` left side; title = derivative label (§5.3); unit line; format `%.4g`; footer = projection note | P0 |
| 37 | Frame label "Frame i of n | Step | Increment | Step time | Total time" (ref:L1586-1589) | Timeline label: "Increment 7 of 31 · Step "Load" · step time 0.35 · total time 0.35" | P0 |
| 38 | Prev / Play-Pause / Next, speed Slow/Normal/Fast 1000/500/200 ms (ref:L1910-1915, L1990-2008) | Same controls + Home/End; loop at end (as ref:L1998) | P0 |
| 39 | Slider live while dragging if ≤150k triangles, else on release (ref:L1917) | `QSlider::setTracking` by same threshold | P0 |
| 40 | Partial updates when only frame changes (ref:L1961-1988) | Only scalars (and points if deformed) are swapped per increment; no pipeline rebuild | P0 |
| 41 | Click-to-history; drag ≠ click (4 px threshold) (ref:L1728-1738, L1930-1941) | Same 4 px rule; `vtkCellPicker` (element data) / nearest node of picked cell (nodal data); picked marker glyph | P0 |
| 42 | History x-axis total time if monotonic, else frame number (ref:L2027-2031) | Same rule; x-axis toggle "Total time / Step time / Increment" (P1) | P0 |
| 43 | History title "Variable at node 42 (BLOCK-1)" (ref:L2036, L1592-1607) | Same wording; plus parameter/order | P0 |
| 44 | Empty history hint "Click on the model to plot the history at that point" (ref:L2019) | Same text, centered in chart | P0 |
| 45 | Hover value on mesh (ref:L1798) | Status-bar readout on hover (throttled 30 Hz): "Node 42 (BLOCK-1): dU2/dE = −1.2e-7" | P1 |
| 46 | Output selection with select all/none + disk estimate (ref:L2134-2155, L1237-1241) | Configure ▸ Outputs checklist with "All / None" and size estimate in cost preview | P0 |
| 47 | Auto frame stride to keep < 2 GB, user-overridable, logged (ref:L2335-2345) | **Computation never strided by default** (master prompt §5). Display loading may page out-of-core; if a display stride is applied it is shown in the timeline label "showing every 2nd increment — change in Settings" | P1 |
| 48 | Free disk check before extraction (ref:L2396-2406) | Cost preview shows disk need vs free; Run disabled with reason if insufficient | P0 |
| 49 | Cache keyed by path+size+mtime; reuse "This file was read before" (ref:L1100-1107, L2315-2316) | Project cache keyed by INP content hash + dependency hashes; message "These results are already computed. Reusing them." | P0 |
| 50 | Incremental field cache (only missing fields extracted) (ref:L1004-1008) | Resume-from-manifest for jobs/increments/parameters | P0 |
| 51 | Clear cache with confirmation (ref:L2425-2434) | Settings ▸ Storage ▸ "Delete cached results…" with size shown; confirmation names what is kept (projects, original INPs untouched) | P0 |
| 52 | Demo: C3D8 block + S4R/S3 shell + T3D2 truss with label gaps, 2 steps (Load/Unload), RF only at clamped end, PEEQ partial, section points 1/5 (ref:L555-687) | Native analytic demo (simulated adapter, §7 of master prompt) must cover the same visual cases **plus** sensitivities with known analytic values; opens Explore directly in < 5 s | P0 |
| 53 | `--demo`, `--no-browser` CLI flags (ref:L2484-2498) | `abss --demo`, `abss path/model.inp`, `abss project.abss` | P0 |
| 54 | Console fallback when no GUI toolkit (ref:L2437-2481) | Not needed (native GUI). `abss-cli` is a separate tool, out of this spec | — |
| 55 | Progress via `@@P i n` and messages `@@MSG` (ref:L480-487, L1194-1205) | Structured progress events from workers → Run page (§4.4) | P0 |
| 56 | Busy state disables conflicting buttons (ref:L2183-2194) | Enable rules §6 | P0 |
| 57 | Window always on top for 1.5 s at start (ref:L2108-2109) | Not needed; normal activation | — |
| 58 | Uncaught exception handler with log (ref:L2259-2267, L2508-2520) | Global handler: error panel + `logs/crash-<time>.txt`; app stays usable if possible | P0 |

---

## 3. Window architecture

Single `QMainWindow`, title "ABSS — <project or INP name>" (+ " — DEMO (synthetic)").

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Menu: File  View  Run  Help                                              │
├────────┬─────────────────────────────────────────────────────────────────┤
│ Step   │                                                                 │
│ rail   │            QStackedWidget: current page                         │
│ ①Start │                                                                 │
│ ②Param │                                                                 │
│ ③Setup │                                                                 │
│ ④Run   │                                                                 │
│ ⑤Explore│                                                                │
├────────┴─────────────────────────────────────────────────────────────────┤
│ Status bar: [Abaqus ●] [Compute: CPU (OpenMP, 16 threads)] [hover/readout]│
└──────────────────────────────────────────────────────────────────────────┘
```

- **Step rail** (left, 132 px, `QListWidget` in icon+text mode, not a wizard with Back/Next only). Items: "Start", "Parameters", "Set up", "Run", "Results". Item state: available / current / locked (greyed, tooltip says what unlocks it) / attention (amber dot, e.g. "1 missing file"). Users can jump back to any unlocked page; changing parameters after a run shows a banner "Results are from the previous setup. Run again to update." (results are kept, not deleted).
- Each page (except Results) has a footer bar: left = secondary action, right = primary action button (accent color): Parameters → "Next: Set up", Set up → "Next: Run", Run → "Start run".
- Explore (Results) page uses `QMainWindow`-style docks inside a nested `QMainWindow` so docks are movable and the layout persists via `saveState()` per user.

---

## 4. Pages, widgets, states

### 4.1 Start (Welcome)

Widgets:
1. Drop zone (`QFrame`, accepts drops, 2 px dashed border, height 180 px): icon `SP_DialogOpenButton`, text "Drop an Abaqus input file (.inp) here", secondary text "or", button **"Choose file…"** (QFileDialog, filter "Abaqus input (*.inp);;ABSS project (*.abss)"). Drop of other extensions → inline message "ABSS opens .inp input files and .abss projects. This file is a .<ext>." Drop of several files → "Drop one .inp file at a time."
2. **"Try the demo (no Abaqus needed)"** button (secondary style) — always enabled.
3. Recent list (`QListWidget`, max 10): name, folder (elided middle), last opened, state badge ("Results ready", "Run interrupted — can resume", "File moved or deleted" (greyed, right-click "Remove from list")). Empty state: "Projects you open will appear here."
4. **Readiness panel** (`QGroupBox` "This computer"), one row each: status dot (painted) + word + detail + action. Status is never conveyed by color alone (word is always present).

| Row | States (word · detail · action) |
|---|---|
| Abaqus | **Found** · "Abaqus 2024 at C:\SIMULIA\Commands\abaqus.bat" · [Change…] / **Not found** · "Needed only to run new analyses. You can still try the demo and open saved results." · [Locate abaqus.bat…] / **Checking…** (spinner) |
| Abaqus license | **Not checked** · "Checked when you start a run." · [Check now] / **Available** / **Unavailable** · interpreted message from log interpreter · [Show details] / disabled with "Requires Abaqus" if Abaqus not found |
| Compute | "CPU — OpenMP, 16 threads" or "GPU — NVIDIA RTX A4000 (CUDA)" or "CPU — serial (no parallel backend in this build)". Info only; no badges claiming speedup |
| Scratch space | "C:\…\ABSS\scratch — 212 GB free" · [Change…]; amber "Low" if < 10 GB |

Readiness checks run asynchronously at startup; the page is usable immediately.

After a file is chosen: progress line "Reading model.inp…" then automatic transition to **Parameters**. Parse errors stay on Start with an error panel (rule 6), e.g. title "This input file could not be read", cause "Line 214: *ELEMENT has no TYPE= parameter", actions "Open the file at this line (opens in your text editor)", "Check that the file opens in Abaqus/CAE".

Missing dependency dialog (one-time per project, `QDialog`): "This model needs files that were not found", table (Kind: Include / UMAT / UEL / table file, Name, Looked in), per row [Locate…]; buttons "Continue without them" (enabled only when no missing item is required for running — otherwise text "You can browse parameters, but running needs these files") and "Done".

### 4.2 Parameters

Layout: top toolbar; center `QTableView` (model/proxy for search); right info pane (300 px) for the selected row; bottom collapsible 3D mini-preview (P1; P0 = mesh summary text "3 instances · 1 064 elements (C3D8, S4R, S3, T3D2) · 2 steps").

Toolbar: search field (`QLineEdit`, placeholder "Search parameters (name, material, set, file)", Ctrl+F); category filter chips (checkable `QToolButton`s): All · Material · User material (UMAT) · User element (UEL) · Load · Boundary condition · Geometry · Section · Amplitude; toggle "Show unsupported"; counter "3 selected of 47".

Table columns (default visible marked *):
| Col | Content | Notes |
|---|---|---|
| ☐* | checkbox | disabled for Unsupported rows (tooltip = reason) |
| Name* | editable display name, e.g. "Young's modulus E — STEEL" | double-click edits; original name in tooltip |
| Category* | text + small painted icon | |
| Nominal value* | `2.1e+05` (`%.6g`) | read-only |
| Units* | "MPa (from model comment)" / "model units" | never invent units |
| Step perturbation* | "auto (1e-6 relative)" | editable only under "Advanced" toggle |
| Confidence* | badge (below) | |
| Source* | `model.inp:214` (or `include/mat.inp:12`) | click → "Open in text editor" / "Copy location" |
| Applies to | material/set/instance/step | hidden by default |
| Bounds | "> 0 (inferred)" | hidden by default |
| Components | "scalar" / "3 values" | hidden by default |

Confidence badges (text inside badge, color secondary):
- **Known** (green outline): recognized meaning, e.g. E, ν, CLOAD magnitude.
- **Indexed** (grey outline): value position known, meaning unknown — e.g. "UMAT ELASTO_PLASTIC: PROPS[3] = 350". Tooltip: "ABSS can change this value, but cannot know what it means inside your compiled user subroutine."
- **Check** (amber outline): inferred from expressions/includes with a parse warning. Tooltip lists the warning.
- **Unsupported** (red outline, row text secondary): checkbox disabled; tooltip states reason, e.g. "Changing node coordinates in a remeshed region is not supported."

Info pane for selected row: plain sentence "Changing this value changes the elastic stiffness of material STEEL, used by 864 elements in BLOCK-1." (Indexed → "This is the 3rd number passed to user material ELASTO_PLASTIC. ABSS does not know its meaning."); the source snippet (3 lines, monospace, line highlighted); "Applies in steps: Load, Unload".

Expert override (P1): button "Add custom parameter…" → dialog choosing file + line + field index; shows the exact edit diff before accepting.

Empty states: no candidates found → "No adjustable values were found in this model. You can add one manually (Advanced)." Search no matches → "No parameters match "xyz". Clear search".

Primary action **"Next: Set up"** disabled until ≥ 1 row checked (tooltip "Select at least one parameter to study."). Exception: order-0-only (nominal) runs need no parameter — see Set up.

### 4.3 Set up (Configure)

Single scrolling form, `QFormLayout` groups:

**A. What to compute**
- "Derivative order" — segmented `QButtonGroup` of checkable `QToolButton`s "0", "1", "2", "3" (more appear only if the capability matrix lists them). Semantics: selecting k computes all orders 0..k (order 0 always included). Labels under each: 0 "Nominal only", 1 "Rate of change", 2 "Curvature", 3 "Third order".
  - State per button from capability matrix × selected parameter categories: *Verified* = normal; *Experimental* = enabled, amber "Experimental" chip, and a one-time confirmation "Second-order results are experimental: they have not passed every validation test for <category>. They will be marked Experimental everywhere they appear."; *Not available* = disabled, tooltip "Third-order sensitivities are not available yet: they have not passed validation tests. See Help ▸ What is verified."
  - If a selected parameter category supports a lower maximum order than others, the button tooltip says "Available for Material parameters, not for Geometry parameters (2 selected)" and the run computes the supported order per parameter; the cost preview lists it.
- Plain-language explainer under the group (secondary text): "Order 1 answers: how much does each output change when the parameter increases a little? Order 2 tells how that rate itself changes."
- Mixed derivatives (P2): checkbox "Also compute cross-effects between pairs of parameters" — **hidden** until the backend supports it.

**B. Outputs** — checklist: "Displacement (U)", "Reaction force (RF)", "Stress (S, incl. Mises)", "Strain (E / LE)", "Plastic strain (PE, PEEQ)", "State variables (SDV)", "Energies". Only outputs present in the model's output requests are enabled; others disabled with tooltip "Not requested in this input file's *OUTPUT definitions." Buttons All / None. Default: U, RF, S.

**C. Increments** — radio: "Every accepted increment (recommended)" (default) / "Only the last increment of each step". No stride option for computation.

**D. Advanced** (collapsed `QToolButton` disclosure "Advanced settings"): finite-difference scheme (combo from backend), relative step h (spin, log), tolerance, max concurrent Abaqus jobs (spin 1..license limit, default 1), CPU threads, compute device combo (only detected devices), scratch folder, max disk use (GB).

**E. Cost preview** (always visible, right column card, recomputed live within 100 ms; computed by backend function, not UI arithmetic):
```
Estimated work
  Abaqus analyses:        1 nominal + 6 perturbed histories
  Abaqus evaluations:     ~ 186 short single-increment jobs
  Relative cost:          about 7–9 × your original analysis
  Disk space:             ~ 3.4 GB of 212 GB free
  Abaqus licenses:        runs 1 job at a time (change in Advanced)
This is an estimate of work, not a time promise.
```
Warnings in the card (amber): "Order 2 roughly doubles the number of Abaqus analyses." / "Disk space is not sufficient" (red; disables Run).

Primary action **"Next: Run"** (enabled when at least one output is checked and disk is sufficient).

### 4.4 Run

- Header line: big status text — "Ready to run" / "Running — nominal analysis" / "Paused" / "Finished" / "Stopped" / "Failed".
- Buttons (large, 40 px high): **Start run** (accent, `SP_MediaPlay`, F5), **Pause** (`SP_MediaPause`; pauses *between* Abaqus jobs; text after click "Pausing after the current Abaqus job…"), **Cancel** (`SP_BrowserStop`, confirmation: "Stop the run? Abaqus jobs started by ABSS will be stopped. Other Abaqus jobs on this computer are not affected. Finished parts are kept so you can resume."). Resume replaces Start when a manifest exists: "Resume run (68 % done)".
- Overall `QProgressBar` with text "68 % — 126 of 186 evaluations".
- Task tree (`QTreeView`): Nominal analysis (increment 31/31 ✓) ▸ per parameter ▸ per order: "E — STEEL, order 1: increment 17 of 31"; status column words: Waiting / Running / Done / Failed / Skipped (with reason). Live checks column (P1): "Residual check passed (2.1e-9)" / amber "Predictor check large at increment 12".
- Current activity line: "Abaqus is solving increment 17 (step "Load", time 0.85)". Sparse solver line: "Solving stiffness system: 12 400 unknowns, direct factorization reused".
- "Show log" disclosure → monospace `QPlainTextEdit` (read-only, 10 000-line ring), buttons "Copy", "Open log folder".
- On failure: error panel (rule 6) above the tree, e.g. title "Abaqus could not converge in the perturbed analysis for E — STEEL", cause "Increment 23 needed more cutbacks than allowed.", actions: "Try a smaller step size (Advanced settings)", "Run the other parameters anyway" [button], "Show details".
- On finish: banner "Results are ready" + button **"Open results"** (also auto-switch if the user has not navigated away). Results from partially failed runs open with failed items marked "No data — run failed" in selectors.
- Navigation during a run: other pages are viewable; Parameters/Set up are read-only with banner "Editing is locked while a run is in progress."

### 4.5 Results (Explore)

```
┌─────────────── "What to show" bar (top, full width) ───────────────────────────────┐
│ Output [U: U2 ▾]  Where [Nodes ▾]  Parameter [E — STEEL ▾]  Order [0][1][2][3]      │
│ "How much does the vertical displacement U2 change when E (STEEL) increases?"       │
├─────────────┬────────────────────────────────────────────────┬─────────────────────┤
│ Model dock  │  3D view (QVTKOpenGLNativeWidget)              │ Display dock        │
│ (left)      │  view toolbar: Fit Iso XY XZ YZ Persp/Ortho    │ (right)             │
│             │  color bar left, axes widget bottom-left       │                     │
├─────────────┴────────────────────────────────────────────────┴─────────────────────┤
│ Timeline: |◀ ◀ ▶Play ▶ ▶| speed[Normal▾] ─────●───────────  Increment 7 of 31 · … │
├────────────────────────────────────────────────────────────────────────────────────┤
│ History chart dock (bottom, 240 px)                                                 │
└────────────────────────────────────────────────────────────────────────────────────┘
```

**What to show bar** (P0):
- *Output* `QComboBox`, grouped with separators: "Displacement: Magnitude, U1, U2, U3", "Stress: Mises, S11…", "Reaction force …", "State variables: SDV1…". Items not computed for the current parameter/order are shown disabled with "(not computed)".
- *Where* combo: "Nodes", "Elements (average of integration points)", "Section point 1 (bottom)", "Section point 5 (top)"; P1 adds "Integration point k".
- *Parameter* combo: display names from Parameters page; disabled when Order = 0 (tooltip "Order 0 shows the nominal result, which does not depend on a parameter choice."). Experimental items get suffix "— experimental".
- *Order* segmented buttons 0..max computed; an order that was not computed is disabled with tooltip "Not computed in this run. Set up ▸ Derivative order." Experimental orders show amber chip.
- *Question sentence* (secondary text, updates live), templates in §5.3.

**3D view** (P0): `QVTKOpenGLNativeWidget`, `vtkGenericOpenGLRenderWindow`, gradient background (§8). Scalar bar actor on the left. Orientation axes (`vtkOrientationMarkerWidget`) bottom-left. Pick marker: small sphere glyph in accent color, labeled with the point number "A", "B"… for multi-pick. Overlay text top-left when demo: "DEMO — synthetic data". View toolbar (floating `QToolBar` top-right of view): Fit (F), Iso (I), Front XY (1), Top XZ (2), Side YZ (3), Perspective/Orthographic toggle (P), Screenshot (Ctrl+Shift+S).

**Model dock** (left, P0 minimal): tree Instance ▸ element type rows "C3D8 × 864 — 3D solid" with visibility checkboxes (P1: checkbox; P0: read-only tree); Steps list with increment counts; Parameters studied with status (Verified/Experimental).

**Display dock** (right):
- Group "Shape": "Show deformed shape" checkbox; "Scale" spin (disabled when unchecked); [Auto] (P1); "Show undeformed outline" (P1).
- Group "Mesh": "Show element edges"; "Wireframe" (P1); "Transparency" slider 0–80 % (P1).
- Group "Colors": "Color map" combo with gradient swatch icons (painted 64×12); "Reverse" checkbox (P1); "Range" radio: "All increments", "This increment", "Fixed"; Min/Max `QLineEdit` with `QDoubleValidator` (enabled only for Fixed; pre-filled with current range on switch); "Centered at zero" checkbox (auto-checked and default for signed quantities, §8.5); "Scale" combo Linear / Logarithmic / Symmetric log (P1; Log disabled with tooltip "Log scale needs all values > 0" when min ≤ 0); "Bands" spin 0 (smooth) / 6–24 (P1, Abaqus-like banded contours).
- Group "Post-processing" (P2): "Time derivative d/dt" and "Spatial gradient ∇" toggles, labelled "computed from the displayed data, not a parameter derivative"; hidden until implemented.

**Timeline** (P0): `|◀` first (Home), `◀` prev (←), Play/Pause (Space), `▶` next (→), `▶|` last (End), speed combo Slow/Normal/Fast (1000/500/200 ms), `QSlider` with step-boundary tick marks, label "Increment 7 of 31 · Step "Load" · step time 0.35 · total time 0.35". Play loops. Changing increment updates 3D view + chart cursor.

**History chart** (P0): `QChartView` (QtCharts), `QLineSeries` + markers. 
- Empty state text: "Click on the model to plot the history at that point. Ctrl+click adds more points."
- Title: "dU2/dE at node 42 (BLOCK-1)"; Y-axis title = label + unit line; X-axis "Total time" (or "Increment" when not monotonic). Vertical dashed cursor at current increment; clicking the chart moves the timeline to nearest increment.
- Multi-pick: Ctrl+click adds up to 8 points (series colors from categorical palette §8.4, legend "A: node 42", …); plain click replaces. "Clear" button removes all.
- Checkbox "Also show nominal value" adds the order-0 history on a secondary right axis (P1).
- Hover on series → `QToolTip` "t = 0.35, dU2/dE = −1.234e-07" (P0).
- Step boundaries drawn as light vertical lines labeled with step name (P1).
- Context menu / buttons: "Copy data", "Export CSV…", "Save image (PNG/SVG)…" (P0 CSV+PNG; SVG P1).
- Experimental order: series dashed + legend suffix "(experimental)".

**Export** (File ▸ Export, P0 subset): Screenshot PNG; current history CSV; current field at current increment CSV; project save. VTU/VTM, video, HTML report: P1/P2.

---

## 5. Labels, notation and wording

### 5.1 Derivative notation (decision)
- Pure single-parameter **total** derivatives (the main ABSS product): `dU2/dE`, `d²U2/dE²`, `d³U2/dE³`, `dS_Mises/dE`, `d²S_Mises/dE²`, `dRF2/dF`, `dSDV3/dPROPS[2]`.
- `∂` is reserved for mixed derivatives (`∂²U2/∂E∂ν`, P2) and for any partial quantity shown in diagnostics (e.g. "pseudoload ∂r/∂E").
- This **deviates** from the master prompt example `∂U2/∂E`: the stored quantity is a total derivative along the equilibrium history (master prompt §3 says "distinguish total vs partial derivatives clearly"). Tooltip on every label: "Total derivative: includes how the whole loading history responds to the change."
- Order 0 label: "U2 (nominal)".
- Rendering: Unicode (∂ U+2202, ² ³ ⁴ superscripts, ∇ U+2207). Fonts: Segoe UI contains these glyphs; implementers verify on Linux fallback font in a GUI test.
- Parameter symbol in notation = short symbol (E, ν, F, PROPS[3]); full name in sentence. If two selected parameters share a symbol, append material: `dU2/dE(STEEL)`.

### 5.2 Units
Abaqus is not unit-aware. Unit line under legend title: if both units known → "mm per MPa"; else "model units of U2 per model unit of E". Never fabricate units.

### 5.3 Question sentence templates
| Order | Template | Example |
|---|---|---|
| 0 | "What is the {output_plain} {where_plain}?" | "What is the vertical displacement U2 at the nodes?" |
| 1 | "How much does the {output_plain} change when {param_plain} increases?" | "How much does the vertical displacement U2 change when Young's modulus E (STEEL) increases?" |
| 2 | "How does that rate of change itself change as {param_plain} increases?" | |
| 3 | "How does the second-order rate change as {param_plain} increases? (third order)" | |
Plain names table (P0): U→"displacement", U1/U2/U3→"displacement in X / vertical (Y) / Z", "U: Magnitude"→"total displacement", S Mises→"von Mises stress", S11→"normal stress S11", RF→"reaction force", PEEQ→"equivalent plastic strain", SDVk→"state variable SDV{k}". Unknown keys → the key itself.

### 5.4 Glossary tooltips (exact text)
- "Increment": "One solution point Abaqus accepted during the analysis."
- "Sensitivity": "How much a result changes per unit change of a parameter."
- "Experimental": "Computed, but not yet confirmed by every validation test. Use with care."
- "Indexed parameter": as §4.2.
- "Element average": "Abaqus computes this value at points inside each element. The colors show their average for display only; the stored data keeps each point."

---

## 6. Enable/disable rules (no inert controls)

| Control | Enabled when | Otherwise (tooltip / behavior) |
|---|---|---|
| Rail: Parameters | INP parsed | "Open an input file first." |
| Rail: Set up | Parameters page visited and model parsed | same |
| Rail: Run | Set up valid | "Finish the setup first: <first unmet item>" |
| Rail: Results | any results loaded (demo, project, finished/partial run) | "No results yet. Run an analysis or try the demo." |
| Next: Set up | ≥1 parameter checked **or** order 0 intended (link "Only compute the nominal result") | "Select at least one parameter to study." |
| Order button k | capability ≠ Not available for ≥1 selected parameter | reason text from capability matrix |
| Start run | Abaqus found, required dependencies present, disk sufficient, no run active | first failing reason; with Abaqus missing: "Abaqus is needed to run new analyses. Locate it on the Start page." |
| Pause / Cancel | run active | hidden when idle (not disabled) |
| Check license | Abaqus found | "Requires Abaqus." |
| Parameter combo (Results) | order ≥ 1 | see §4.5 |
| Min/Max fields | Range = Fixed | — |
| Centered at zero | data has both signs or quantity is a sensitivity | "Only useful when values can be negative and positive." |
| Log scale | all finite values > 0 | "Log scale needs all values greater than zero." |
| Deformation scale | deformed shape on and U exists | — |
| Show deformed shape | U nominal exists | "This result has no displacement (U) output." |
| Timeline controls | ≥2 increments | single increment: controls hidden, label "Single increment" |
| Play | ≥2 increments | — |
| Export history / Copy data | ≥1 picked point | "Click on the model first to choose a point." |
| Clear (picks) | ≥1 picked point | — |
| Screenshot | results shown | — |

---

## 7. Keyboard shortcuts (P0)

| Key | Action | Scope |
|---|---|---|
| Ctrl+O | Open input file / project | global |
| Ctrl+Shift+D | Open demo | global |
| Ctrl+S / Ctrl+Shift+S | Save project / Save screenshot | global / Results |
| Ctrl+1 … Ctrl+5 | Go to Start / Parameters / Set up / Run / Results (if unlocked) | global |
| Ctrl+F | Focus parameter search | Parameters |
| Space | Toggle parameter checkbox | Parameters table |
| F5 | Start / resume run | global (when enabled) |
| Shift+F5 | Cancel run (with confirmation) | global |
| Space | Play / pause | Results (3D view or timeline focused) |
| ← / → | Previous / next increment | Results |
| Home / End | First / last increment | Results |
| F / I / 1 / 2 / 3 / P | Fit / Iso / XY / XZ / YZ / perspective toggle | 3D view focused |
| E | Toggle element edges | 3D view focused |
| D | Toggle deformed shape | 3D view focused |
| Esc | Clear picked points | Results |
| F1 | Help (local HTML/markdown in app) | global |

All controls reachable by Tab in visual order; every button has an accessible name (`setAccessibleName`) equal to its tooltip's first sentence; 3D view gets focus on click and shows a 2 px accent focus ring.

---

## 8. Visual style guide

### 8.1 Palette tokens (QSS + `QPalette`)
Contrast ratios computed (WCAG 2.x relative luminance) on 2026-10-16 for text tokens on their panel color.

| Token | Light | Dark | Use / contrast |
|---|---|---|---|
| `window` | #F6F7F9 | #1E2126 | page background |
| `panel` | #FFFFFF | #262A30 | cards, docks, tables |
| `border` | #D9DCE1 | #3A3F47 | 1 px separators |
| `text` | #1C1E21 | #E6E8EB | 16.7:1 / 11.8:1 |
| `text2` | #5B616B | #A0A6AF | secondary; 6.2:1 / 5.9:1 |
| `disabled` | #8A9099 | #7A808A | disabled text (3.2:1 / 3.6:1; exempt, always paired with tooltip) |
| `accent` | #1F5FBF | #5B9BFF | primary buttons, focus, picks; 6.1:1 text-on-white / dark text #1E2126 on accent 5.8:1 |
| `ok` | #2A7A2A | #5CB85C | "Found", "Verified"; 5.4:1 / 5.8:1 |
| `warn` | #A15C00 | #E0A040 | "Experimental", "Check"; 5.2:1 / 6.4:1 |
| `error` | #B03030 | #F06A6A | failures; 6.3:1 / 4.8:1 |
| `nodata` | #C9CCD1 | #5A5F66 | NaN color in 3D |
| `edges` | #30343A | #0E1013 | mesh edges |
| `bg3d_top` → `bg3d_bottom` | #FFFFFF → #E9ECF0 | #2B2F36 → #1A1C20 | 3D gradient |
| `demo_banner` | bg #FFF4E0, text #7A4500 | bg #3A2E1A, text #F2C27A | synthetic banner |

Theme: follow OS (Qt 6.5+ `QStyleHints::colorScheme`), override in View ▸ Theme (System / Light / Dark). Base style "Fusion" for consistent rendering; palette set via `QPalette`, details via one QSS file `resources/abss.qss` with `@token` placeholders substituted at load.

### 8.2 Spacing and sizes
4 px grid. Inner widget spacing 8; group spacing 12; page margins 16; dock margins 8. Controls 28 px high; primary buttons 32 px (Run page 40 px). Corner radius 6 (buttons, cards), 4 (inputs, badges). Min window 1100×720; default 1400×900; layouts usable at 1280×800 at 150 % scaling (test). High-DPI: `Qt::AA_EnableHighDpiScaling` default in Qt6; icons generated at device pixel ratio.

### 8.3 Typography
System UI font (Segoe UI on Windows, default on Linux) — no bundled downloads. Base 10 pt; secondary 9 pt; group titles 10 pt semibold; page titles 15 pt semibold; question sentence 11 pt; monospace (logs, source snippets, paths) "Cascadia Mono", fallback "Consolas", then `QFontDatabase::systemFont(FixedFont)`. Numbers in tables right-aligned, `%.6g`; legend/hover `%.4g`.

### 8.4 Icons
No external icon files or downloads. Use `QStyle::standardIcon`: SP_DialogOpenButton (open), SP_DialogSaveButton (save), SP_MediaPlay/Pause/SeekBackward/SeekForward/SkipBackward/SkipForward (timeline), SP_BrowserStop (cancel), SP_BrowserReload (resume), SP_MessageBoxInformation/Warning/Critical (banners), SP_FileDialogDetailedView (log), SP_DirOpenIcon (open folder). Painted at runtime with `QPainter` (one `IconFactory` class): status dots, view icons (cube wireframe with highlighted face for Iso/XY/XZ/YZ, four-arrow box for Fit), colormap gradient swatches, category glyphs (letter in rounded square: M, U, L, B, G, S, A — with text label always adjacent). Categorical chart colors (multi-pick, 8): #1F5FBF, #D9730D, #2A9D8F, #B03060, #6A4C93, #8C6D1F, #3B8EA5, #5B616B (dark theme: lighten 20 %).

### 8.5 Colormaps (P0 list, embedded as 256-entry tables in source)
Viridis, Plasma, Inferno, Cividis, Turbo, Cool–warm (Moreland), Blue–white–red (seismic-like, RdBu reversed), Greys, Rainbow (Abaqus-like). Ada: record the license of each table (matplotlib's viridis family CC0; Turbo Apache-2.0; Moreland cool-warm public description) in `third_party/NOTICE`. User-defined palettes (load from a small CSV of RGB stops): P1.

Defaults (decision):
| Quantity shown | Default map | Range |
|---|---|---|
| Order 0, non-negative (Mises, magnitudes, PEEQ) | Viridis | All increments |
| Order 0, signed (U2, S11, RF2) | Cool–warm, centered at zero | All increments, symmetric |
| Order ≥ 1 (any sensitivity) | Cool–warm, **centered at zero** | All increments, symmetric ±max\|v\| |
The user's explicit choice of map persists per (output, order) for the session. Centered mode: `vtkLookupTable` range [−m, m], m = max|v| over the chosen range mode; legend shows 0 tick explicitly. Zero is the white/neutral midpoint, so sign is readable at a glance.

---

## 9. Usability test script (to run against the built app)

**Status:** protocol only. No participants have been run. A Council member or AI persona walking these tasks is a *cognitive walkthrough* and must be reported as such, not as usability evidence.

Persona/recruit: 4–6 people; materials/mechanical researchers who run Abaqus through Abaqus/CAE, have not written FE code, are unfamiliar with "Jacobian/tangent". Exclude ABSS developers. Setup: Windows machine with built ABSS; Abaqus present for T6–T10 (else run T1–T5, T11–T12 only and record the gap); example `examples/` INPs including one with a UMAT. Think-aloud; moderator gives only the task card, no hints for 2 minutes of being stuck; then a scripted hint ("Where would you look for…") counts as an assist. Record: completion (yes / with assist / fail), time, errors, assists, and post-task ease 1–7 (Single Ease Question). Screen recording only with consent; no personal data stored beyond role.

| # | Task card (read aloud) | Success criterion (observable) | Pri |
|---|---|---|---|
| T1 | "Without using Abaqus, open an example and tell me whether the numbers are real Abaqus results." | Opens demo in ≤ 60 s; states it is synthetic, pointing at a label | P0 |
| T2 | "Where on the model does the vertical displacement react most strongly to the stiffness parameter at the last increment?" | Selects U2, order 1, parameter E, last increment; points at the region of largest \|value\|; correctly says whether it increases or decreases | P0 |
| T3 | "Show how that sensitivity evolves over the loading at the point you chose, and read its value at the end of the first step." | Click-picks a point; reads value within legend precision; ≤ 1 assist | P0 |
| T4 | "Compare two points on the same chart." | Uses Ctrl+click (or discovers it from empty-state text) | P1 |
| T5 | "Make the colors use the range −1e-6 to 1e-6 and then go back to automatic." | Sets Fixed, enters values, returns to All increments | P0 |
| T6 | "Open examples/plate.inp and choose the elastic modulus of STEEL and the applied load." | Finds both via search or category filter; checks them | P0 |
| T7 | "What does PROPS[3] of the user material mean?" | States that ABSS doesn't know / it's the 3rd user-material constant (correct); fails if participant believes ABSS knows its physical meaning | P0 |
| T8 | "Set it up to compute first-order sensitivities and tell me roughly how much work it is before you start." | Reads the cost preview aloud (analyses count, disk); starts run | P0 |
| T9 | "Third order would be useful — can you get it?" | Finds order 3 disabled and explains why from tooltip (not "the app is broken") | P0 |
| T10 | "Stop the run, then continue it later." | Cancels with confirmation; reopens project; resumes; no duplicate work shown | P0 |
| T11 | (Abaqus path deliberately unset) "Run this model." | Reads readiness/disabled-run reason; locates abaqus.bat or states what is needed; ≤ 1 assist | P0 |
| T12 | "Save the chart as a picture and the numbers as a spreadsheet file." | Produces PNG and CSV; CSV opens in Excel with header incl. parameter, order, units line | P0 |
| T13 | Keyboard only: "Play the animation, step back two increments, fit the view." | Completes without mouse | P1 |

Release thresholds (proposed, for Atlas/Santiago to accept): each P0 task completed without assist by ≥ 4 of 5 participants; no participant misreads synthetic as real (T1) or believes an opaque PROPS meaning is known (T7) or that a disabled order is a bug (T9) — any single such misreading is a blocker requiring a design change and retest. Median SEQ ≥ 5 for P0 tasks. Every observed failure is recorded as "design issue", with the interaction that caused it, not as user error.

---

## 10. Walkthrough findings on the reference viewer (designer walkthrough, not observed)

Ranked friction points the native app must not repeat:
1. Integration-point values silently averaged per element (ref:L936-956) — risk of misreading point values. Fix §2 #22.
2. Manual min/max inputs always editable even when mode ≠ manual (ref:L1894-1897) — inert-looking controls. Fix §6.
3. Default Turbo/Jet colormap for all variables, no centered diverging option — sign of a sensitivity unreadable. Fix §8.5.
4. Automatic frame stride for large models (ref:L2335-2345) noted only in the log — users may not notice skipped increments. Fix §2 #47.
5. Only one picked point; no hover readout on chart — comparisons need repeated clicks. Fix §4.5.
6. Workflow split across Tk window + browser tab; "keep this window open" dependency (ref:L2237). Fix: single window.
7. First run requires internet + pip (ref:L186-285). Fix: bundled native install.
8. No unit information anywhere. Fix §5.2.
Strengths to preserve: plain-language step numbering ("1. Choose…, 2. Read…"), exact location hints in errors, drag-vs-click discrimination, camera preservation, "no data" grey, demo needing no Abaqus, one deletable app folder.

---

## 11. Open questions (need owner decisions)
1. Notation `d` vs `∂` (§5.1) — Noether/Atlas to confirm; affects all labels and exports.
2. Does the capability matrix expose per-category × per-order × per-output status? UI assumes yes (rule 4). Ada/Gauss.
3. License check: is there a non-consuming way to probe availability (`abaqus licensing` variants differ by version)? Otherwise keep "Checked when you start a run". Ada with Abaqus expert.
4. Cost-preview model: who owns the job-count formula per order/scheme (Gauss), and can relative cost be bounded honestly before a nominal run exists (time grid unknown until the nominal run)? Before nominal run, show counts per increment ("~6 jobs per increment") instead of totals.
5. Pause semantics with license queueing: pause between jobs only (assumed).
6. Whether order-0-only (nominal view) runs are in scope for first release (assumed yes, they cost one Abaqus job).
