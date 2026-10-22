# Suggested replacement slides (NASA STRI weekly)

All numbers are measured on 2026-10-21/22. Figures are in `figures/`, as PDF, SVG and PNG. "Single run" means the
value has no repeats.

---

## 1. Question and answer

**Question.** When does ABSS give accurate sensitivities at a cost that competes with full-analysis finite
differences?

**Answer (measured):**
- **8 Abaqus jobs at a time:** FAFD is faster in every case: elastic meshes up to 111k DOFs, P = 1–33, J2
  plasticity and Ti-6242. No crossover.
- **1 job at a time:** ABSS with replicated branches is faster for P ≥ 4 on meshes up to 2,400 DOFs. The crossover is
  bracketed between P = 2 and P = 4, and between 2,400 and 15,552 DOFs, with 3 interleaved repeats.
- **Accuracy:** ABSS agrees with FAFD to ≤ 7e-7 of each derivative's natural size, on every field, increment and
  point, including all 132 Ti-6242 SDVs.

---

## 2. Where the time goes

Figure: `A_breakdown_j8`, plus this table.

| Phase of an Abaqus job (average of 6,065 jobs) | Seconds |
|---|---|
| Driver + licence | 6.3 |
| Input processing | 3.2 |
| Abaqus/Standard (≤ 2,400 DOFs) | 5.0 |
| Wrap-up | 1.3 |

- About 11 s per job is fixed overhead, the same for both methods.
- ABSS's own compute is about 14 s of summed worker time in a 101–111 s run (A48).
- **Job counts, P = 4, 4 increments:**
  - ABSS one job per branch: 53;
  - ABSS replicated: 40 at 8 slots, 5 at 1 slot;
  - FAFD: 8;
  - ABSS before the changes: 80.

---

## 3. What was improved, and how it was verified

Figure: `before_after`.

| Change | Verified by |
|---|---|
| Probe and commit waves run concurrently: 2 per increment instead of 5 | Unit test, maximum difference 0 |
| Each commit job also probes the next increment (restart from the end of a step) | Abaqus experiment, 7e-16 relative; whole run ≤ 2e-8 |
| No ODB output in branch jobs | `.fil` bitwise identical |
| Validation reruns in parallel | Same reruns, run concurrently |
| GMRES right-hand sides solved concurrently | Unit test, maximum difference 0 |
| True tangent from grouped probes (81 groups on hex meshes), with a row-wise check and GMRES fallback | Validated |
| Replicated branches: several branches as disjoint model copies in one job | Two-copy Abaqus test bitwise; whole runs bitwise |

**Result: 2–14× faster than the starting point on identical cases, with the same derivatives.**
- A16, 1 slot: 1,075 → 78 s.
- Ti-6242 with 56 parameters: 6,362 → 398 s.

---

## 4. Mesh scaling (family A, elastic, P = 4, 8 slots)

Figures: `A_time_vs_dofs_j8`, `A_speedup_vs_dofs_j8`.

- T_FAFD / T_ABSS is 0.20 at 432 DOFs and 0.09–0.13 at 48k–111k DOFs, with 2 repeats.
- **No crossover. A larger mesh does not help ABSS:** each ABSS job reads and writes the full restart state, which
  grows faster than an elastic solve.

---

## 5. Parameter count and increments (family A, 8 slots, single runs)

Figures: `A_time_vs_params_j8`, `A_speedup_vs_params_j8`, `A_time_vs_increments_j8`.

- **Parameter count:** replicated ABSS stays almost flat in P while FAFD grows. T_FAFD / T_ABSS rises from 0.16 to
  0.32 at P = 33, but stays below 1.
- **Increments:** ABSS grows linearly with increments; FAFD is almost flat. T_FAFD / T_ABSS falls from 0.49 to 0.06
  at 16 increments.

---

## 6. The measured crossover: one Abaqus job at a time

Figures: `S_speedup_vs_params_j1`, `S_speedup_vs_dofs_j1`. Three interleaved repeats.

| Case | T_FAFD / T_ABSS replicated |
|---|---|
| 432 DOFs, P = 1 | 0.36 |
| 432 DOFs, P = 2 | 0.74 |
| 432 DOFs, P = 4 | **1.49** |
| 432 DOFs, P = 8 | **2.83** |
| P = 4, 2,400 DOFs | **1.23** |
| P = 4, 15,552 DOFs | 0.50 |

Replicated ABSS needs N + 1 jobs whatever P is; FAFD needs 2P. The ratio matters when the licence or the machine
allows only one job at a time.

---

## 7. Plasticity and Ti-6242 (8 slots, single runs)

Figures: `B_time_vs_dofs_j8`, `B_accuracy_vs_dofs`.

- **J2 load/unload:** FAFD is 22–87× faster. Abaqus exports the elastic stiffness for plasticity, so ABSS corrects
  the tangent at every plastic increment: GMRES (65–182 products) or an assembled tangent.
- **Ti-6242:** FAFD is 7–77× faster. At P = 56 (including inactive slots), the ratio is 0.56, the closest to parity.
  Replicated ABSS is almost flat in P there. A crossover near P ≈ 100 is a **hypothesis**, not measured.
- **One flagged derivative:** at 7,007 DOFs, the yield-stress derivative for segment 3 hits yield onset inside the FD
  stencil. ABSS flagged it as non-smooth (consistency diagnostic 2.2e-3, against about 1e-5 elsewhere).

---

## 8. Accuracy

Figures: `A_accuracy_vs_dofs`, `B_accuracy_vs_dofs`, and the step-size table in REPORT §4.

- **Metric:** max |ABSS − FAFD| / (max|F| / |a|), over all increments and points. Dividing by max|FAFD| is
  meaningless where the true derivative is about 0.
- **Family A:** 5e-9 to 7e-7. **Family B:** ≤ 2e-7, except the one flagged derivative. **Ti:** ≤ 1.4e-7, all 132
  SDVs included.
- **Step size:** both methods converge to the same derivative. ABSS is insensitive to h from 1e-4 to 1e-6. FAFD's
  round-off grows at h = 1e-6.
- **Erratum:** the earlier Ti "SDV validated" compared zeros with zeros (a `.fil` reader bug). It is fixed and now
  verified.

---

## 9. Recommendation

- **Several Abaqus jobs at once:** use FAFD for first-order sensitivities on these model classes. ABSS ships it as a
  clearly labelled backend (`abss-cli fafd`). FAFD output is labelled FAFD, never ABSS.
- **One job at a time** (licence-limited), with P ≥ 4 on small or medium meshes: use ABSS with `--batch`.
- **ABSS adds:** per-increment diagnostics (tangent and predictor consistency, non-smooth flags), built-in
  validation, higher-order and mixed derivatives, and the GUI.
- **Not yet tested:** analyses whose perturbed reruns fail or need different increments, and strongly nonlinear
  analyses with many Newton iterations per increment.
