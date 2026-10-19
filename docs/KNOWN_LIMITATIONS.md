# Known limitations

The app enforces each of these by refusing to run, disabling the control, or labelling the results. None of them is
hidden behind fabricated output.

## Physics and procedures (Abaqus backend)
- **Procedures.** Only `*STATIC` (general, incl. NLGEOM) steps are supported. RIKS, `*DYNAMIC`, `*VISCO`, heat
  transfer, coupled procedures and linear perturbation steps are refused with a message.
- **Constraints and contact.** `*MPC`, `*EQUATION`, `*TIE`, `*COUPLING`, `*KINEMATIC COUPLING`, `*RIGID BODY`,
  `*CONTACT PAIR` and `*CONTACT` are refused. The reaction-force oracle on independent DOFs
  (Noether §2.3) is derived but not validated against Abaqus. See ABAQUS_REQUIRED_TESTS C5–C6.
- **Changing BC sets.** DOFs whose constrained status changes between steps (a BC added later, or released by
  `OP=NEW`) are refused.
- **Model structure.** The Abaqus backend needs a flat deck (no `*PART`/`*INSTANCE`). The parser and the GUI handle
  part/instance decks, so they can still be inspected and their parameters viewed.
- **Loads and amplitudes.** Loads other than `*CLOAD`/`*DLOAD`/`*DSLOAD` are refused, as are non-TABULAR amplitudes
  and `VALUE=ABSOLUTE` amplitudes. The re-evaluated BC/CLOAD history is self-checked against the nominal job; a
  mismatch above 1e-6 aborts the run.
- **Tangent under plasticity.** Abaqus provides only the elastic-modulus stiffness under built-in plasticity. The
  engine detects this and switches to matrix-free GMRES, which costs two Abaqus probe jobs per operator application.
  This is fine for validation-size models but expensive for large plastic models.
- **Time grid.** Derivatives are those of the algorithm on the frozen nominal time grid. Validation reruns require a
  uniform increment within each step; otherwise validation is reported as not possible.
- **Shells and rotations.** Rotational DOFs are prescribed as Abaqus UR values. Whether a prescribed UR under NLGEOM
  equals a total rotation vector is unverified (H-UR), so shell results are Unverified until test C3 passes.
- **User subroutines.** UMAT/UEL subroutines are treated as opaque; only `PROPS[k]` are differentiable, and
  `JPROPS`/`*DEPVAR` are info-only. Validated on Abaqus 2024 only for small elastic examples (a UMAT cube and a UEL
  truss, order 1). A UMAT with plasticity, a UEL with internal DOFs, and orders ≥ 2 with user subroutines have not
  been run. Building from source or `.obj` needs Intel Fortran (ifort or ifx) and the MSVC linker. The direct build
  (used when Abaqus lacks its user-subroutine development files) was tested with ifx 2025.1 and Abaqus 2024 only.
  `/iface:cref` sets the export names, so a C/C++ subroutine must use lowercase unmangled names (`extern "C" void
  umat(...)`). A prebuilt `standardU.dll` must match the Abaqus release that loads it.
- **Cost.** Every probe and commit is an Abaqus job (about 13 s of start-up each on the development PC). One
  increment at order K with P parameters needs about `2 + P·2M·(K+1)` jobs (plus tangent checks and Krylov
  operator applications), run up to `--jobs N` at a time. Large models with many increments are expensive; the app
  shows the job count before Run.

## Mathematics
- **Non-smooth events.** Yield onset or reversal and contact changes make derivatives one-sided or nonexistent.
  ABSS flags them (commit-residual spikes, tangent-check failure, re-probe non-convergence) and does not smooth them
  away. One-sided derivatives are not computed.
- **Step size.** There is a narrow usable window between stencils that straddle kinks (h too large) and noise
  ~ tol/h^k (h too small). Defaults are 1e-5/1e-4/1e-3 relative for K = 1/2/3. Automatic step-size sweeps are not
  implemented; use `--rel-step` and compare.
- **Orders 3 and above.** These run, but are always labelled Experimental. Order 3 has been validated only on the
  analytic truss (synthetic).
- **Mixed derivatives.** Only second-order mixed derivatives are exposed.
- **Validation metric granularity.** The error is normalised by each field's largest reference value (with a
  physical floor). Small components of a field, for example a shear component much smaller than the normal stresses,
  are therefore not bounded separately (Vera F8).
- **Validation evidence.** On Abaqus, validation uses a single step size per run. Step-shrinking sweeps (spec §7)
  have been done only on the synthetic models.
- **Load replay check.** DLOAD/DSLOAD replay is not self-checked against Abaqus output, because CF does not include
  distributed loads. Only BC values and concentrated loads are cross-checked.
- **Geometry BCs.** BCs are taken as displacements. A BC meaning "fixed absolute position" would need `g' = −X'`,
  which is not implemented.

## Software
- **Kokkos.** It is used on the host (OpenMP) for stencil combination, SpMV/SpMM and reductions. GPU backends
  (CUDA/HIP) are possible by rebuilding Kokkos, but have not been built or measured. Kokkos Kernels is not used.
  Jacobi-PCG under Kokkos is *slower* than serial at n ≈ 2·10⁴ (launch overhead; measured 0.23×).
- **Direct solvers.** Eigen SparseLU/LDLT and SuiteSparse UMFPACK are used. No MKL/PARDISO and no Trilinos solvers.
- **Synthetic demo.** It uses an in-house small-strain FE model (truss, plane-strain quad, hex; J2 plasticity) and
  is labelled "synthetic" everywhere. It is a test bed, not an Abaqus emulator for user models.
- **Remote schedulers.** HPC Slurm adapters are not implemented.
- **Export.** Exports are CSV, VTU (from the GUI) and the `.abss` results file. HDF5, PDF report and video export are
  not implemented.
