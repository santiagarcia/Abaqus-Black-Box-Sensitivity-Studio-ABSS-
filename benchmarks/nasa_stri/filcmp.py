"""Compare nodal U (101) and RF (104) records of the last increment of two ASCII Abaqus .fil files.

Usage:  python filcmp.py a.fil b.fil [stepA incA stepB incB]
"""
import re
import sys


def records(path):
    text = "".join(l.rstrip("\n") for l in open(path))
    for rec in text.split("*")[1:]:
        vals, p = [], 0
        while p < len(rec):
            c = rec[p]
            if c == "I":
                w = int(rec[p + 1:p + 3])
                vals.append(int(rec[p + 3:p + 3 + w]))
                p += 3 + w
            elif c == "D":
                vals.append(float(rec[p + 1:p + 23].replace("D", "E")))
                p += 23
            elif c == "A":
                vals.append(rec[p + 1:p + 9])
                p += 9
            else:
                p += 1
        if len(vals) >= 2:
            yield vals[1], vals[2:]


def increments(path):
    out, cur = {}, None
    for key, v in records(path):
        if key == 2000:  # increment start: total time, step time, creep rate, amplitude, procedure, step, increment
            cur = (int(v[5]), int(v[6]))
            out[cur] = {}
        elif key in (101, 104) and cur:
            out[cur][(key, int(v[0]))] = v[1:]
    return out


a, b = increments(sys.argv[1]), increments(sys.argv[2])
ka = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else max(a)
kb = (int(sys.argv[5]), int(sys.argv[6])) if len(sys.argv) > 6 else max(b)
worst, scale = 0.0, 0.0
for k, va in a[ka].items():
    vb = b[kb].get(k)
    if vb is None:
        print("missing", k)
        continue
    for x, y in zip(va, vb):
        worst = max(worst, abs(x - y))
        scale = max(scale, abs(x))
print(f"{sys.argv[1]} {ka} vs {sys.argv[2]} {kb}: {len(a[ka])} records, max |diff| {worst:.3e}, max |value| {scale:.3e}")
