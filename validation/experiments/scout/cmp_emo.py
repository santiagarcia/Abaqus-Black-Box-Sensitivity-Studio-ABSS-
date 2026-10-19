import json
from emo_lib import read_emo, assemble
E = read_emo("t4_emo.mtx")
for inc in (4, 7):
    G = assemble(E[(1, inc)])
    Ke = [[G[(3, a, 3, b)] for b in (1, 2)] for a in (1, 2)]
    d = json.load(open("exp_truss_restart_i%d.json" % inc))
    fd1 = d["fd"]["+e1_eps1e-07"]["col"]; fd2 = d["fd"]["-e2_eps1e-07"]["col"]
    Kfd = [[fd1[0], fd2[0]], [fd1[1], fd2[1]]]
    print("inc", inc)
    print("  EMO   ", Ke)
    print("  FD    ", Kfd)
    print("  MG    ", d["K_matrix_generate"])
    print("  relerr EMO vs FD", max(abs(Ke[i][j]-Kfd[i][j]) for i in range(2) for j in range(2))/abs(Kfd[0][0]))
print(sorted(E.keys()))
