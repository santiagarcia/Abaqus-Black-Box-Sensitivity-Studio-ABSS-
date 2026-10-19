"""Parse *ELEMENT MATRIX OUTPUT (OUTPUT FILE=USER DEFINED) .mtx files and assemble them.

Each block: comment header with element/step/increment, '** ELEMENT NODES' + '**  n1, n2,...',
active DOF line, '*MATRIX,TYPE=STIFFNESS', then lower-triangular rows (node-major DOF order).
"""
import re


def read_emo(path):
    """Return dict[(step, inc)] -> list of (elem, nodes, dofs, dense lower-tri filled matrix)."""
    out = {}
    lines = open(path).read().splitlines()
    i = 0
    hdr = re.compile(r"\*\* ELEMENT NUMBER\s+(\d+)\s+STEP NUMBER\s+(\d+)\s+INCREMENT NUMBER\s+(\d+)")
    while i < len(lines):
        m = hdr.match(lines[i].strip())
        if not m:
            i += 1
            continue
        el, st, inc = (int(x) for x in m.groups())
        while not lines[i].strip().upper().startswith("** ELEMENT NODES"):
            i += 1
        nodes = [int(x) for x in lines[i + 1].replace("**", "").split(",") if x.strip()]
        dofs = [int(x) for x in lines[i + 2].split(",") if x.strip()]
        i += 3
        while not lines[i].upper().startswith("*MATRIX"):
            i += 1
        i += 1
        n = len(nodes) * len(dofs)
        vals = []
        while len(vals) < n * (n + 1) // 2:
            vals += [float(x) for x in lines[i].split(",") if x.strip()]
            i += 1
        K = [[0.0] * n for _ in range(n)]
        k = 0
        for r in range(n):
            for c in range(r + 1):
                K[r][c] = K[c][r] = vals[k]
                k += 1
        out.setdefault((st, inc), []).append((el, nodes, dofs, K))
    return out


def assemble(blocks):
    """Assemble element blocks into dict[(n1,d1,n2,d2)] -> value (no BCs applied)."""
    G = {}
    for el, nodes, dofs, K in blocks:
        keys = [(n, d) for n in nodes for d in dofs]
        for a, ka in enumerate(keys):
            for b, kb in enumerate(keys):
                G[ka + kb] = G.get(ka + kb, 0.0) + K[a][b]
    return G
