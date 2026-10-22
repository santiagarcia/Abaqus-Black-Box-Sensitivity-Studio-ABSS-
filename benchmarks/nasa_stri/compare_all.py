"""For every ABSS run (any ABSS method) with a matching FAFD run (same case and slots; same repeat, else repeat 1),
write the field-by-field agreement (abss-cli compare, FAFD as reference) to cmp_<case>__<method>_j<jobs>_r<k>.json.

Usage:  python compare_all.py <abss-cli> <results dir> [...]
"""
import glob
import os
import subprocess
import sys

cli = sys.argv[1]
for d in sys.argv[2:]:
    for a in sorted(glob.glob(os.path.join(d, "*__abss*_j*_r*.abss"))):
        case, rest = os.path.basename(a)[:-5].split("__")
        method, jr = rest.rsplit("_j", 1)
        jobs, rep = jr.split("_r")
        f = os.path.join(d, f"{case}__fafd_j{jobs}_r{rep}.abss")
        if not os.path.exists(f):
            f = os.path.join(d, f"{case}__fafd_j{jobs}_r1.abss")
            if not os.path.exists(f):
                continue
        out = os.path.join(d, f"cmp_{case}__{method}_j{jr}.json")
        if os.path.exists(out):
            continue
        subprocess.run([cli, "compare", f, a, "--json", out], stdout=subprocess.DEVNULL, check=False)
        print(out)
