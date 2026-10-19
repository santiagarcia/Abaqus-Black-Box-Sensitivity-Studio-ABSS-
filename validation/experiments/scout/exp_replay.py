import json
from scout_lib import read_fil, run_job
base = read_fil("t3_base.fil")
r6 = [r for r in base if r["inc"] == 6][0]; r7 = [r for r in base if r["inc"] == 7][0]
u7 = r7["U"][3]
inp = """*HEADING
replay inc 7 from restart at inc 6 with all DOFs prescribed to u7*
*RESTART, READ, STEP=1, INC=6, END STEP
*FILE FORMAT, ASCII
*STEP, NAME=REPLAY, NLGEOM=YES
*STATIC, DIRECT
0.1, 0.1
*BOUNDARY, OP=NEW
FIX, 1, 2
3, 1, 1, %.17e
3, 2, 2, %.17e
*CLOAD, OP=NEW
TIP, 1, 70.
TIP, 2, -140.
*NODE FILE
U, RF
*EL FILE
S, PE, LE
*END STEP
""" % (u7[0], u7[1])
open("t7_replay.inp", "w").write(inp)
print(run_job("t7_replay", oldjob="t3_base"))
rr = read_fil("t7_replay.fil")[-1]
out = {"RF3_replay": rr["RF"][3], "RF1_replay": rr["RF"][1], "RF1_base_inc7": r7["RF"][1],
       "S2_replay": rr["EL"][(2, 1)]["S"], "S2_base_inc7": r7["EL"][(2, 1)]["S"],
       "PEEQ2_replay": rr["EL"][(2, 1)]["PE"][-1], "PEEQ2_base_inc7": r7["EL"][(2, 1)]["PE"][-1],
       "time": [rr["total_time"], rr["step_time"]]}
json.dump(out, open("t7_replay.json", "w"), indent=1); print(json.dumps(out, indent=1))
