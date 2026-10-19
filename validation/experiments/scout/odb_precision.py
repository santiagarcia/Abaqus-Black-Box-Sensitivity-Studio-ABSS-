"""Compare ODB field precision (single vs output_precision=full) against .fil doubles.
Usage: abaqus python odb_precision.py  (expects t5_full.odb, t5_single.odb, t5_full.fil)
"""
import sys
import time

t0 = time.time()
from odbAccess import openOdb  # noqa: E402

t_import = time.time() - t0
from scout_lib import read_fil  # noqa: E402

fil = read_fil("t5_full.fil")
print("python", sys.version.split()[0], "odbAccess import %.2fs" % t_import)
for name in ("t5_single.odb", "t5_full.odb"):
    t1 = time.time()
    odb = openOdb(name, readOnly=True)
    step = odb.steps["LOAD"]
    worst = {"U": 0.0, "RF": 0.0, "S": 0.0}
    prec = {}
    for fr in [f for f in step.frames if f.incrementNumber > 0]:
        inc = fr.incrementNumber
        ref = [r for r in fil if r["inc"] == inc][0]
        for key in ("U", "RF"):
            fo = fr.fieldOutputs[key]
            prec[key] = str(fo.values[0].precision)
            for v in fo.values:
                d = v.dataDouble if str(v.precision) == "DOUBLE_PRECISION" else v.data
                rv = ref[key][v.nodeLabel]
                for a, b in zip(d, rv):
                    if abs(b) > 1e-12:
                        worst[key] = max(worst[key], abs(float(a) - b) / abs(b))
        fo = fr.fieldOutputs["S"]
        prec["S"] = str(fo.values[0].precision)
        for v in fo.values:
            d = v.dataDouble if str(v.precision) == "DOUBLE_PRECISION" else v.data
            rv = ref["EL"][(v.elementLabel, v.integrationPoint)]["S"]
            worst["S"] = max(worst["S"], abs(float(d[0]) - rv[0]) / abs(rv[0]))
    peeq = step.frames[-1].fieldOutputs["PEEQ"].values[0]
    prec["PEEQ"] = str(peeq.precision)
    print(name, "precision:", prec, "max rel diff vs .fil:", worst,
          "time %.2fs for %d frames" % (time.time() - t1, len(step.frames)))
    odb.close()

