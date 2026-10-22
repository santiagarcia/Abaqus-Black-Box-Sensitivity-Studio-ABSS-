"""Benchmark decks for the ABSS vs FAFD scaling campaign (stdlib only; deterministic output).

Family A  elastic C3D8 cantilever, 100 x 10 x 10, clamped at x=0, uniform pressure on the top face (bending).
          16 axial segments, each with its own material (E_i, nu_i): every parameter is genuinely active.
Family B  same geometry, J2 plasticity with linear hardening per segment; prescribed tip deflection loads the beam
          into the plastic range and partially unloads it (history dependent, smooth, no contact, no instability).
          Fixed increments (*STATIC, DIRECT) so both methods use the same time grid by construction.

Usage:  python gen_models.py <outdir>
"""
import os
import sys

L, W, H = 100.0, 10.0, 10.0
SEGMENTS = 16
# (nx, ny, nz): nx divisible by SEGMENTS
MESHES = [(16, 2, 2), (32, 4, 4), (48, 6, 6), (64, 8, 8), (96, 12, 12), (128, 16, 16)]


def grid(nx, ny, nz):
    nid = lambda i, j, k: 1 + i + (nx + 1) * (j + (ny + 1) * k)
    nodes = [(nid(i, j, k), L * i / nx, W * j / ny, H * k / nz)
             for k in range(nz + 1) for j in range(ny + 1) for i in range(nx + 1)]
    elems = []
    eid = 0
    seg = {s: [] for s in range(SEGMENTS)}
    top = []
    for k in range(nz):
        for j in range(ny):
            for i in range(nx):
                eid += 1
                n = [nid(i, j, k), nid(i + 1, j, k), nid(i + 1, j + 1, k), nid(i, j + 1, k),
                     nid(i, j, k + 1), nid(i + 1, j, k + 1), nid(i + 1, j + 1, k + 1), nid(i, j + 1, k + 1)]
                elems.append((eid, n))
                seg[i * SEGMENTS // nx].append(eid)
                if k == nz - 1:
                    top.append(eid)
    root = [nid(0, j, k) for k in range(nz + 1) for j in range(ny + 1)]
    tip = [nid(nx, j, k) for k in range(nz + 1) for j in range(ny + 1)]
    return nodes, elems, seg, top, root, tip


def idlist(ids, per=16):
    return "\n".join(", ".join(str(x) for x in ids[i:i + per]) for i in range(0, len(ids), per))


def deck(family, nx, ny, nz, nInc):
    nodes, elems, seg, top, root, tip = grid(nx, ny, nz)
    out = ["*HEADING",
           f"ABSS benchmark family {family}: C3D8 cantilever {nx}x{ny}x{nz}, {SEGMENTS} material segments, {nInc} increments",
           "*NODE"]
    out += [f"{n}, {x:.10g}, {y:.10g}, {z:.10g}" for n, x, y, z in nodes]
    out.append("*ELEMENT, TYPE=C3D8, ELSET=ALL")
    out += [f"{e}, " + ", ".join(map(str, n)) for e, n in elems]
    for s in range(SEGMENTS):
        out += [f"*ELSET, ELSET=SEG{s + 1:02d}", idlist(seg[s])]
    out += ["*ELSET, ELSET=TOPL", idlist(top), "*NSET, NSET=ROOT", idlist(root), "*NSET, NSET=TIP", idlist(tip)]
    out += ["*SURFACE, NAME=TOPF", "TOPL, S2"]
    for s in range(SEGMENTS):
        out += [f"*SOLID SECTION, ELSET=SEG{s + 1:02d}, MATERIAL=M{s + 1:02d}", ","]
    for s in range(SEGMENTS):
        # mild, deterministic variation so segments are distinguishable
        E = 70000.0 * (1.0 + 0.01 * s)
        nu = 0.30 + 0.002 * s
        out += [f"*MATERIAL, NAME=M{s + 1:02d}", "*ELASTIC", f"{E:.6g}, {nu:.6g}"]
        if family == "B":
            sy = 250.0 * (1.0 + 0.01 * s)
            out += ["*PLASTIC", f"{sy:.6g}, 0.", f"{sy + 1500.0:.6g}, 0.1"]
    if family == "A":
        dt = 1.0 / nInc
        out += ["*STEP, NAME=BEND, NLGEOM=NO", "*STATIC, DIRECT", f"{dt:.10g}, 1.",
                "*BOUNDARY", "ROOT, ENCASTRE", "*DSLOAD", "TOPF, P, 1.",
                "*OUTPUT, FIELD", "*NODE OUTPUT", "U, RF", "*ELEMENT OUTPUT", "S, E", "*END STEP"]
    else:
        # tip deflection: first yield at the root at d_y = sy L^2 / (1.5 E H) = 2.38; load to 6.0 (~2.5 d_y), unload to 3.0
        half = max(1, nInc // 2)
        out += ["*STEP, NAME=LOAD, NLGEOM=NO", "*STATIC, DIRECT", f"{1.0 / half:.10g}, 1.",
                "*BOUNDARY", "ROOT, ENCASTRE", "TIP, 3, 3, -6.0",
                "*OUTPUT, FIELD", "*NODE OUTPUT", "U, RF", "*ELEMENT OUTPUT", "S, E, PE, PEEQ", "*END STEP",
                "*STEP, NAME=UNLOAD, NLGEOM=NO", "*STATIC, DIRECT", f"{1.0 / max(1, nInc - half):.10g}, 1.",
                "*BOUNDARY, OP=MOD", "TIP, 3, 3, -3.0",
                "*OUTPUT, FIELD", "*NODE OUTPUT", "U, RF", "*ELEMENT OUTPUT", "S, E, PE, PEEQ", "*END STEP"]
    return "\n".join(out) + "\n", len(elems), 3 * nx * (ny + 1) * (nz + 1)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "decks"
    os.makedirs(outdir, exist_ok=True)
    rows = []
    for fam, incs in (("A", (1, 4, 16)), ("B", (4, 8, 16))):
        for (nx, ny, nz) in MESHES:
            for n in incs:
                text, ne, nfree = deck(fam, nx, ny, nz, n)
                name = f"{fam}_{nx}x{ny}x{nz}_inc{n}.inp"
                with open(os.path.join(outdir, name), "w", newline="\n") as f:
                    f.write(text)
                rows.append(f"{name},{fam},{nx},{ny},{nz},{ne},{8 * ne},{nfree},{n}")
    with open(os.path.join(outdir, "decks.csv"), "w", newline="\n") as f:
        f.write("deck,family,nx,ny,nz,elements,integration_points,free_dofs,increments\n" + "\n".join(rows) + "\n")
    print(f"{len(rows)} decks written to {outdir}")


if __name__ == "__main__":
    main()
