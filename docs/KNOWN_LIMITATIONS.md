# Known limitations

The app enforces each of these by refusing to run, disabling the control, or labelling the results. None of them is
hidden behind fabricated output.

## Physics and procedures (Abaqus backend)
- **Procedures.** `*STATIC`, `*VISCO` and implicit `*DYNAMIC` (HHT/Newmark, or backward Euler for the quasi-static
  application) are supported, including NLGEOM. Refused, with the reason: `*STATIC, RIKS` (the load factor is an
  unknown of the arc-length method, so there is no fixed load path to differentiate along), `*DYNAMIC, EXPLICIT`,
  heat transfer and coupled procedures (their residual is not a displacement residual), and linear perturbation steps.
- **Dynamics.** With every DOF prescribed, Abaqus leaves inertia out of the reactions (measured), so ABSS adds
  `M a(u)` itself: it uses Abaqus's element mass matrices (per branch, so density and geometry parameters are exact)
  and the step's integrator, with the α/β/γ values read from the nominal `.dat`. A dynamic model that also has
  constraints is refused: the inertia of dependent DOFs reaches their masters only through constraint equations
  Abaqus does not expose. Rayleigh and element damping are not modelled separately. The run-time replay check marks
  the results Failed if replaying the nominal analysis does not reproduce equilibrium.
- **Constraints.** `*TIE`, `*EQUATION`, `*MPC`, couplings, rigid bodies and embedded elements are supported. One
  extra "DOF check" job finds the DOFs that Abaqus eliminates: every candidate DOF is prescribed with a distinct tiny
  value, and Abaqus ignores the BC on a dependent DOF. The dependent DOFs are then left to Abaqus in every job.
  Abaqus does not expose its constrained (condensed) stiffness, so these models are solved matrix-free with GMRES,
  at two Abaqus jobs per operator application, which is slower.
- **Contact.** Contact forces are part of `-RF`, so contact runs through the same oracle. Derivatives are those of
  the current contact state. An opening/closing or stick/slip change inside a stencil is non-smooth and is flagged
  per increment. Not yet validated on an Abaqus contact model.
- **Changing BC sets.** BCs added in a later step or released by `OP=NEW` are supported: the unknowns are set per
  step, and a released DOF carries its own branch's reaction force, ramped down as Abaqus does (RAMP) or removed at
  once (STEP). Not yet validated on Abaqus.
- **Model structure.** Flat and part/assembly/instance decks are supported. For an assembly, the global numbering
  that Abaqus uses in its output is mapped to instance and label, and checked against Abaqus's own model records
  (connectivity must match exactly). Steps and assemblies inside `*INCLUDE` files are supported: every generated
  job uses a flattened copy with all includes inlined.
- **Loads, fields and amplitudes.** Supported: `*CLOAD` (including FOLLOWER), `*DLOAD`, `*DSLOAD`, step
  `*TEMPERATURE` (ramped from the initial temperature), and amplitudes TABULAR (incl. SMOOTH), SMOOTH STEP, EQUALLY
  SPACED, PERIODIC, MODULATED and DECAY, with VALUE=RELATIVE or ABSOLUTE. Refused: SOLUTION DEPENDENT, BUBBLE,
  USER and ACTUATOR amplitudes, and temperatures read from a results file or user code, because ABSS cannot evaluate
  them at its own increments. The re-evaluated BC/CLOAD history is self-checked against the nominal job; a
  mismatch above 1e-6 aborts the run. DLOAD/DSLOAD/temperature replay is not cross-checked by that test, but the
  engine's replay check covers it.
- **Thermal expansion.** Measured on Abaqus 2024: element-matrix output in a model with `*EXPANSION` makes Abaqus
  repeat its first increment forever. Such models get their element matrices from a separate job without
  `*EXPANSION`; expansion does not change the stiffness, and the matrices only precondition the solver.
- **Constrained parameter sets.** Some compiled materials couple their constants. For example, two fractions of the
  Ti-6242 library must not sum above 1, and raising one alone returns NaN state variables. ABSS reports such a NaN
  with the job, the quantity and the parameter. The meaningful derivative is along a linked direction: the CLI
  accepts `combo:<id>*<w>+<id>*<w>` (one scalar moving several values, e.g. keeping a sum fixed). The GUI does
  not offer linked directions yet.
- **Inconsistent tangents.** Abaqus's element matrices are not the consistent tangent under built-in plasticity,
  built-in hyperelasticity with NLGEOM (measured tangent check ≈ 0.19), for most UMATs, or for constrained and
  dynamic models (measured ≈ 0.01–0.03). The engine detects this and switches to matrix-free GMRES, which costs
  two Abaqus probe jobs per operator application. This is accurate (validated to 1e-6..1e-8) but slow on large
  models.
- **Nominal accuracy.** The nominal job runs with tight equilibrium controls (R_n = 1e-8), because every derivative
  is linearised at the nominal states. With Abaqus's default 5e-3, nonlinear sensitivities were limited to
  1e-4..1e-3 (measured). If Abaqus cannot converge that tightly, the defaults are used and the run log says so.
- **Time grid.** Derivatives are those of the algorithm on the frozen nominal time grid. Validation reruns require a
  uniform increment within each step; otherwise validation is reported as not possible.
- **Finite rotations.** Measured on Abaqus 2024 (H-UR resolved): under NLGEOM, a rotation BC in a new step is
  applied as the increment of the BC value, composed in space (R = exp(b − b_prev) R_prev). The output UR is the
  total rotation vector. ABSS writes `b = b_prev + log(R(φ) R(φ_prev)ᵀ)` so that every restart job reaches the
  intended total rotation (replay residual 1e-8, against 0.27 without the correction).
- **User subroutines.** UMAT/UEL subroutines are treated as opaque; only `PROPS[k]` are differentiable, and
  `JPROPS`/`*DEPVAR` are info-only. Validated on Abaqus 2024 only for small elastic examples (a UMAT cube and a UEL
  truss, order 1). A UMAT with plasticity, a UEL with internal DOFs, and orders ≥ 2 with user subroutines have not
  been run. Building from source or `.obj` needs Intel Fortran (ifort or ifx) and the MSVC linker. The direct build
  (used when Abaqus lacks its user-subroutine development files) was tested with ifx 2025.1 and Abaqus 2024 only.
  `/iface:cref` sets the export names, so a C/C++ subroutine must use lowercase unmangled names (`extern "C" void
  umat(...)`). A prebuilt `standardU.dll` must match the Abaqus release that loads it.
- **Material bridge (foreign libraries).** A material library that Abaqus cannot load itself runs behind a
  generated UMAT/SDVINI. This covers a Linux `.so` under WSL, and a Windows `.dll` whose interface is either a
  standard UMAT or a deformation-gradient update with its own state initialiser. Every material-point call is
  forwarded through a pipe, which costs about 0.1 ms per call. A Linux `.so` runs natively in ABSS's ELF host
  (`abss_elfhost.exe`, no WSL needed). The host maps the library itself and supplies its imports through System V
  ABI thunks: the C math/memory functions, LAPACK through OpenBLAS, and the gfortran runtime calls material models
  use. Its limits are deliberate and loud:
  - **Input files are not read.** Every INQUIRE answers "does not exist" and every OPEN fails with IOSTAT, and each
    one is logged; libraries whose optional input files have built-in defaults work unchanged.
  - **Stopped with a message** naming the cause: a Fortran READ, thread-local storage, or an import the host does
    not provide. Such libraries run under WSL instead, which stays as the fallback.

  Verified with the vendor Ti-6242 library: the README's 12-increment uniaxial path gives the stress
  [1931.1772, 1473.682, 2457.898, 0, 0, 0] and dσ_zz/dPROPS[43] = 0.5965045 at h = 1e-3…1e-5, both identical to the
  Linux reference values. The bridge was validated in Abaqus with a Windows test library at 1.5e-6.
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

## Platforms
- **Windows** is the tested platform (Abaqus 2024, every verification case).
- **Linux and HPC** run from the self-extracting installer (private conda-forge toolchain, user space, headless on
  clusters). Tested on the Windows build machine: the installer's syntax, its self-extraction, and a POSIX compile
  of every Linux-only code path, including the in-process material-bridge wrapper for both library ABIs. Not yet
  run on a real Linux machine with Abaqus, because none was available.
- **Desktop GUI on Linux** needs conda-forge's VTK to provide Qt support. If it does not, the installer builds the
  command line only and says so.

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
