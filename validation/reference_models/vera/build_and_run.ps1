# Vera's independent probes. Builds against the existing build-core static libraries (does not touch the product build).
# Usage (PowerShell 5.1, from the repo root):  .\validation\reference_models\vera\build_and_run.ps1 [path\to\truss.abss]
param([string]$TrussProject = "")
$ErrorActionPreference = "Stop"
$env:PATH = "C:\Users\vfn333\toolchains\msys64\ucrt64\bin;$env:PATH"
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$b = "$root\build-core"
$k = "C:\Users\vfn333\toolchains\kokkos-openmp"
$u = "C:\Users\vfn333\toolchains\msys64\ucrt64"
$out = Join-Path $env:TEMP "vera_probe.exe"
& g++ -O2 -std=c++20 -fopenmp -DABSS_HAVE_KOKKOS=1 "-I$root\include" "-isystem" "$u\include\eigen3" "-isystem" "$k\include" `
  "$PSScriptRoot\vera_probe.cpp" -o $out `
  "$b\lib\libabss_sensitivity.a" "$b\lib\libabss_oracle.a" "$b\lib\libabss_inp.a" "$b\lib\libabss_core.a" "$b\lib\libabss_linalg.a" `
  "$k\lib\libkokkoscontainers.a" "$k\lib\libkokkosalgorithms.a" "$k\lib\libkokkossimd.a" "$k\lib\libkokkoscore.a" `
  "$u\lib\libgomp.dll.a" "$u\lib\libmingwthrd.a" "$u\lib\libumfpack.dll.a"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }
if ($TrussProject) { & $out $TrussProject } else { & $out }
