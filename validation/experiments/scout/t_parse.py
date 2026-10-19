from scout_lib import read_fil
incs = read_fil("t3_base.fil")
for r in incs:
    print(r["step"], r["inc"], r["total_time"], r["U"].get(3), r["RF"].get(1), {k:v for k,v in r["EL"].items() if k[0]==2})
