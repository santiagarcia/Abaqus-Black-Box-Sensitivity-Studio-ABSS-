"""Write the campaign plans (JSON) for benchmark families A and B and the before/after comparison.

Every plan uses the same parameter set for every method, central differences with the engine's default step, the same
fixed time grid, tolerances and outputs, and matched concurrency (equal job slots, 1 CPU per job) in each comparison.
Methods: "abss" = ABSS with one Abaqus job per branch evaluation (fused commit+probe jobs, concurrent waves);
"abss_batch" = ABSS with replicated branches (--batch); "fafd" = full-analysis central finite differences (baseline);
"abss_before" = the starting point (v1 binary with the original stage-by-stage schedule, --no-overlap).
"""
import json
import os

D = "benchmarks/nasa_stri/decks"
OUT = "benchmarks/nasa_stri/plans"
E = lambda k: f"mat:M{k:02d}:elastic:E"
NU = lambda k: f"mat:M{k:02d}:elastic:nu"
SY = lambda k: f"mat:M{k:02d}:plastic:sigma_y:1"
P1, P4, P16 = [E(1)], [E(k) for k in (1, 2, 3, 4)], [E(k) for k in range(1, 17)]
P33 = [E(k) for k in range(1, 17)] + [NU(k) for k in range(1, 17)] + ["load:BEND:dsload:TOPF:P"]
MESH = ["16x2x2", "32x4x4", "48x6x6", "64x8x8", "96x12x12", "128x16x16"]
METHODS = ["abss_batch", "abss", "fafd"]


def case(cid, deck, params, jobs, repeats, methods=METHODS, **extra):
    c = {"id": cid, "deck": deck if "/" in deck else f"{D}/{deck}", "params": params, "methods": list(methods),
         "jobs": jobs, "repeats": repeats, "bin": "%BIN%", "extra_abss_batch": ["--batch"]}
    c.update(extra)
    return c


plans = {
    # A1: mesh scaling, P = 4, 4 increments, matched 8 slots, interleaved repeats
    "A_mesh_j8": [case(f"A{m}_inc4_P4", f"A_{m}_inc4.inp", P4, [8], 2) for m in MESH],
    # A2: parameter count at 15,552 free DOFs
    "A_params_j8": [case(f"A64x8x8_inc4_P{len(p)}", "A_64x8x8_inc4.inp", p, [8], 2) for p in (P1, P4, P16, P33)],
    # A3: serial reference (1 slot each), single runs
    "A_mesh_j1": [case(f"A{m}_inc4_P4", f"A_{m}_inc4.inp", P4, [1], 1) for m in ("16x2x2", "64x8x8")],
    # A4: increments (secondary study)
    "A_inc_j8": [case(f"A32x4x4_inc{n}_P4", f"A_32x4x4_inc{n}.inp", P4, [8], 1) for n in (1, 4, 16)],
    # B1: nonlinear history-dependent family, P = 4 initial yield stresses of the yielding root segments
    "B_mesh_j8": [case(f"B{m}_inc8_P4", f"B_{m}_inc8.inp", [SY(k) for k in (1, 2, 3, 4)], [8], 1)
                  for m in ("16x2x2", "32x4x4", "48x6x6")],
    # Before/after on identical cases: v1 stage-by-stage schedule vs the current schedules
    "before_after": [
        case("c3d8_E_nu", "examples/models/c3d8_block.inp", ["mat:ALU:elastic:E", "mat:ALU:elastic:nu"], [8], 1,
             methods=["abss_before"] + METHODS, bin_abss_before="%BIN_V1%", extra_abss_before=["--no-overlap"]),
        case("A16_inc4_P4", "A_16x2x2_inc4.inp", P4, [8, 1], 1, methods=["abss_before"] + METHODS,
             bin_abss_before="%BIN_V1%", extra_abss_before=["--no-overlap"]),
        case("B16_inc8_P1", "B_16x2x2_inc8.inp", [SY(1)], [8], 1, methods=["abss_before"] + METHODS,
             bin_abss_before="%BIN_V1%", extra_abss_before=["--no-overlap"]),
    ],
}
os.makedirs(OUT, exist_ok=True)
for name, plan in plans.items():
    json.dump(plan, open(os.path.join(OUT, name + ".json"), "w"), indent=1)
    print(name, len(plan), "cases")
