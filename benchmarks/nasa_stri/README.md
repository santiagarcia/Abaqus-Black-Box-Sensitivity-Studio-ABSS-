# ABSS vs full-analysis finite differences: benchmark campaign

**Question.** Under which measured conditions does ABSS give accurate sensitivities at a cost that competes with
full-analysis finite differences (FAFD)?

The findings are in `REPORT.md`, the profiling and the changes in `../../docs/PERFORMANCE.md`, and the
environment in `results/ENVIRONMENT.md`.

## Methods compared

Every comparison uses the same model, parameter set, step h, derivative order (1), fixed time grid, equilibrium
tolerances and outputs. Comparisons also use the same number of Abaqus job slots, with 1 CPU per job.

| Label | Command | What it is |
|---|---|---|
| `abss` | `abss-cli run` | ABSS with one Abaqus job per branch evaluation: concurrent waves, fused commit+probe jobs |
| `abss_batch` | `abss-cli run --batch` | ABSS with replicated branches. Each wave is one job per slot, and each job holds several branches as disjoint model copies. |
| `abss_before` | v1 binary, `abss-cli run --no-overlap` | The starting point: the original stage-by-stage schedule |
| `fafd` | `abss-cli fafd` | **The baseline, not ABSS:** 2 complete Abaqus analyses per parameter, each on the nominal's time grid. Its output is labelled FAFD. |

## Reproduce

1. Build ABSS (Release), then freeze a copy of `build/bin` (the campaign never runs a binary that is being rebuilt).
2. Generate the decks:
   - `python benchmarks/nasa_stri/gen_models.py benchmarks/nasa_stri/decks` (families A and B, 36 decks);
   - `python benchmarks/nasa_stri/refine_cube.py <cube.inp> <outdir> 1 2 3 4 6` (family C, from your own material
     deck).
3. Write the plans: `python benchmarks/nasa_stri/make_plans.py`.
4. Run the stages:
   `powershell -File benchmarks/nasa_stri/run_campaign.ps1 -Bin <frozen bin> [-BinV1 <baseline bin>] [-Stages ...]`.
   - Runs are interleaved. Repeats alternate the method order, and finished runs are skipped on restart.
5. Analyse:
   - `python benchmarks/nasa_stri/analyze.py <results dirs> --out summary.csv` writes one row per run;
   - `python benchmarks/nasa_stri/compare_all.py <abss-cli> <results dirs>` compares ABSS with FAFD field by field;
   - `python benchmarks/nasa_stri/figures.py summary.csv benchmarks/nasa_stri/results figures` draws the figures.

Each run writes:
- `<case>__<method>_j<slots>_r<repeat>.json` (profile, configuration and diagnostics);
- `.log` (the full console output);
- `.abss` (all derivatives, viewable in the GUI; git-ignored because of size).

The same run's `abss-cli ... --profile run.json` prints:
- elapsed time per phase: setup, nominal, sensitivity, validation and serialization;
- summed worker time and counts per category: every Abaqus job split into driver/licence, input processing,
  analysis and wrap-up, attributed to the activity that requested it;
- engine compute: tangent assembly, factorization and solves;
- per-increment diagnostics: tangent check, GMRES products and residual, and column groups of the assembled tangent;
- the validation status of every derivative.

## Families

| Family | Model | Parameters |
|---|---|---|
| A | Elastic C3D8 cantilever 100 × 10 × 10, 16 axial material segments, top-face pressure. Meshes from 16×2×2 (432 free DOFs) to 128×16×16 (110,976 free DOFs); 1, 4 or 16 increments. | E of segments 1–4 (P = 4). P = 1, 16 or 33 (16 E, 16 ν and the pressure) in the parameter study. |
| B | The same geometry with J2 plasticity and linear hardening. The tip deflection loads the root region well into the plastic range, then partially unloads it (8 fixed increments). | Initial yield stress of the four yielding root segments |
| C | The Ti-6242 compiled UMAT (private) on a refined unit cube, with the same library, properties, BCs and loading. | P = 1, 4 or 13, including the sum-preserving linked PROPS[1]/PROPS[2] direction and only parameters that are active on this loading path |
