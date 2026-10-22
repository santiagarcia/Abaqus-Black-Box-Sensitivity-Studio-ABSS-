"""Refine a single-C3D8 unit-cube deck (nodes 1..8, ELSET=CUBE, NSETs X0/Y0/Z0/TOP) into n x n x n elements.

Everything outside the *NODE ... (last *NSET) mesh block is copied verbatim (material, user-material constants,
DEPVAR, initial conditions, step, BCs, loading, outputs), so the material library, properties, boundary conditions
and loading are identical across the sequence; only the mesh changes.

Usage:  python refine_cube.py <cube.inp> <outdir> n1 n2 ...
"""
import os
import sys


def mesh_block(n):
    nid = lambda i, j, k: 1 + i + (n + 1) * (j + (n + 1) * k)
    out = ["*NODE"]
    for k in range(n + 1):
        for j in range(n + 1):
            for i in range(n + 1):
                out.append(f"{nid(i, j, k)}, {i / n:.12g}, {j / n:.12g}, {k / n:.12g}")
    out.append("*ELEMENT, TYPE=C3D8, ELSET=CUBE")
    e = 0
    for k in range(n):
        for j in range(n):
            for i in range(n):
                e += 1
                c = [nid(i, j, k), nid(i + 1, j, k), nid(i + 1, j + 1, k), nid(i, j + 1, k),
                     nid(i, j, k + 1), nid(i + 1, j, k + 1), nid(i + 1, j + 1, k + 1), nid(i, j + 1, k + 1)]
                out.append(f"{e}, " + ", ".join(map(str, c)))
    face = {
        "X0": [nid(0, j, k) for k in range(n + 1) for j in range(n + 1)],
        "Y0": [nid(i, 0, k) for k in range(n + 1) for i in range(n + 1)],
        "Z0": [nid(i, j, 0) for j in range(n + 1) for i in range(n + 1)],
        "TOP": [nid(i, j, n) for j in range(n + 1) for i in range(n + 1)],
    }
    for name, ids in face.items():
        out.append(f"*NSET, NSET={name}")
        out += [", ".join(map(str, ids[p:p + 16])) for p in range(0, len(ids), 16)]
    return out


def main():
    src, outdir, ns = sys.argv[1], sys.argv[2], [int(a) for a in sys.argv[3:]]
    lines = open(src).read().splitlines()
    first = next(i for i, l in enumerate(lines) if l.upper().startswith("*NODE"))
    # end of mesh block: first keyword after *NODE that is not *NODE/*ELEMENT/*NSET/*ELSET (or a data line of them)
    end = next(i for i in range(first, len(lines)) if lines[i].startswith("*") and not lines[i].startswith("**")
               and lines[i].split(",")[0].strip().upper() not in ("*NODE", "*ELEMENT", "*NSET", "*ELSET"))
    os.makedirs(outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(src))[0]
    for n in ns:
        text = lines[:first] + mesh_block(n) + lines[end:]
        text[1] = f"{text[1]} [refined {n}x{n}x{n}]"
        path = os.path.join(outdir, f"{base}_{n}x{n}x{n}.inp")
        open(path, "w", newline="\n").write("\n".join(text) + "\n")
        free = 3 * (n + 1) ** 3 - 4 * (n + 1) ** 2
        print(f"{path}: {n ** 3} elements, {8 * n ** 3} integration points, {free} free DOFs")


if __name__ == "__main__":
    main()
