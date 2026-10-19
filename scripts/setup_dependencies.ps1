# One-command setup: detect every dependency, download/install what is missing, then build and test ABSS.
#
#   powershell -ExecutionPolicy Bypass -File scripts\setup_dependencies.ps1                   # build toolchain + ABSS
#   powershell -ExecutionPolicy Bypass -File scripts\setup_dependencies.ps1 -UserSubroutines  # + Intel Fortran & MSVC
#
# Everything except the optional compilers installs user-locally under %USERPROFILE%\toolchains (no admin rights).
# -UserSubroutines installs Visual Studio 2022 Build Tools (C++) and Intel oneAPI HPC Toolkit (Fortran) with winget;
# Windows shows an administrator prompt for each. They are only needed to BUILD UMAT/UEL from source or .obj; a
# prebuilt standardU.dll runs without them. Abaqus itself is licensed software and is only detected, never installed.
param(
    [switch]$UserSubroutines,
    [switch]$SkipBuild,
    [string]$Toolchains = "$env:USERPROFILE\toolchains"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$msys = Join-Path $Toolchains "msys64"
$bash = Join-Path $msys "usr\bin\bash.exe"
$ucrt = Join-Path $msys "ucrt64\bin"
$kokkosVersion = "5.2.2"
$kokkosPrefix = Join-Path $Toolchains "kokkos-openmp"
$packages = "gcc cmake ninja qt6-base qt6-charts qt6-svg qt6-tools vtk eigen3 suitesparse nlohmann-json doctest" -split " " |
    ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Ok($msg) { Write-Host "   OK  $msg" -ForegroundColor Green }
function Todo($msg) { Write-Host "   ->  $msg" -ForegroundColor Yellow }
function Bash($cmd) {
    & $bash -lc $cmd
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 command failed ($LASTEXITCODE): $cmd" }
}
New-Item -ItemType Directory -Force $Toolchains | Out-Null

# ---- 1. MSYS2 UCRT64 (compiler, Qt 6, VTK, Eigen, SuiteSparse, ...) ------------------------------------------------
Step "MSYS2 UCRT64 toolchain"
if (-not (Test-Path $bash)) {
    Todo "not found - downloading MSYS2 into $msys"
    $sfx = Join-Path $Toolchains "msys2-base.sfx.exe"
    Invoke-WebRequest -UseBasicParsing https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe -OutFile $sfx
    & $sfx -y "-o$Toolchains\" | Out-Null
    Remove-Item $sfx
    & $bash -lc "true" | Out-Null          # first start initialises the keyring
    Bash "pacman -Syu --noconfirm" ; Bash "pacman -Syu --noconfirm"
}
Ok "MSYS2 at $msys"
$missing = @()
foreach ($p in $packages) { & $bash -lc "pacman -Q $p >/dev/null 2>&1"; if ($LASTEXITCODE -ne 0) { $missing += $p } }
if ($missing.Count) {
    Todo "installing $($missing.Count) package(s): $($missing -join ' ')"
    Bash "pacman -S --noconfirm --needed $($missing -join ' ')"
}
Ok "packages: Qt 6, VTK, Eigen, SuiteSparse, nlohmann_json, doctest"

# ---- 2. Kokkos with the OpenMP backend (MSYS2's Kokkos is Serial-only) --------------------------------------------
Step "Kokkos $kokkosVersion (OpenMP)"
if (-not (Test-Path (Join-Path $kokkosPrefix "lib\cmake\Kokkos"))) {
    Todo "not found - downloading and building (a few minutes)"
    $tc = ($Toolchains -replace "\\", "/") -replace "^([A-Za-z]):", { "/" + $_.Groups[1].Value.ToLower() }
    $pfx = ($kokkosPrefix -replace "\\", "/")
    Bash ("export PATH=/ucrt64/bin:`$PATH; cd '$tc' && " +
          "curl -sSLo kokkos-$kokkosVersion.tar.gz https://github.com/kokkos/kokkos/releases/download/$kokkosVersion/kokkos-$kokkosVersion.tar.gz && " +
          "tar xzf kokkos-$kokkosVersion.tar.gz && " +
          "cmake -S kokkos-$kokkosVersion -B kokkos-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 " +
          "-DKokkos_ENABLE_OPENMP=ON -DKokkos_ENABLE_SERIAL=ON -DCMAKE_INSTALL_PREFIX='$pfx' && " +
          "cmake --build kokkos-build -j && cmake --install kokkos-build")
}
Ok "Kokkos at $kokkosPrefix"

# ---- 3. Abaqus (detected only) --------------------------------------------------------------------------------------
Step "Abaqus"
$abq = @($env:ABSS_ABAQUS_CMD, "C:\SIMULIA\Commands\abaqus.bat", "C:\Program Files\SIMULIA\Commands\abaqus.bat") |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($abq) { Ok "found $abq" } else { Todo "not found: install Abaqus (licensed) or set ABSS_ABAQUS_CMD. The demo models work without it." }

# ---- 4. Optional: compilers for user subroutines ------------------------------------------------------------------
if ($UserSubroutines) {
    Step "User-subroutine toolchain (Microsoft C++ linker + Intel Fortran)"
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = $null
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $vs) {
        if (-not $winget) { throw "winget is not available: install 'Visual Studio 2022 Build Tools' (C++ workload) manually." }
        Todo "installing Visual Studio 2022 Build Tools (C++) - approve the administrator prompt"
        winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-package-agreements `
            --accept-source-agreements --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vs) { throw "Visual Studio Build Tools installation did not complete." }
    }
    Ok "Microsoft C++ tools at $vs"
    $oneapi = @($env:ONEAPI_ROOT, "${env:ProgramFiles(x86)}\Intel\oneAPI") | Where-Object { $_ -and (Test-Path (Join-Path $_ "setvars.bat")) } | Select-Object -First 1
    $fortran = $null
    if ($oneapi) { $fortran = Get-ChildItem (Join-Path $oneapi "compiler") -Recurse -Include ifort.exe, ifx.exe -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if (-not $fortran) {
        # winget cannot raise the Intel installer's elevation prompt from a non-interactive shell (0x800704c7), so the
        # official offline installer is downloaded, hash-checked against the winget manifest, and started elevated.
        Todo "installing Intel oneAPI HPC Toolkit (Fortran compiler, ~2.8 GB) - approve the administrator prompt"
        $url = "https://registrationcenter-download.intel.com/akdlm/IRC_NAS/77bf3a15-a766-4619-91b4-1011badc4a36/intel-oneapi-hpc-toolkit-2025.1.3.7_offline.exe"
        $sha = "ead9cf59a11e411ebd670e51ca7fa27f8679e158fc4d70b85b0f0f361ada56f6"
        $inst = Join-Path $Toolchains "installers"
        New-Item -ItemType Directory -Force $inst | Out-Null
        $exe = Join-Path $inst (Split-Path $url -Leaf)
        if (-not (Test-Path $exe)) { Start-BitsTransfer -Source $url -Destination $exe }
        if ((Get-FileHash $exe -Algorithm SHA256).Hash -ine $sha) { Remove-Item $exe; throw "Intel installer hash mismatch; download removed." }
        $p = Start-Process -FilePath $exe -ArgumentList "-s -a --silent --eula accept" -Verb RunAs -PassThru -Wait
        # Success is decided by detection below; a non-zero code can be a warning (integration/restart notice).
        if ($p.ExitCode -ne 0) { Todo "Intel installer reported code $($p.ExitCode); checking whether the compiler is present" }
        $oneapi = "${env:ProgramFiles(x86)}\Intel\oneAPI"
        $fortran = Get-ChildItem (Join-Path $oneapi "compiler") -Recurse -Include ifort.exe, ifx.exe -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $fortran) { throw "Intel Fortran installation did not complete." }
    }
    Ok "Intel Fortran: $($fortran.FullName)"
    if ($fortran.Name -eq "ifx.exe") { Ok "ifx detected: ABSS configures Abaqus to use it automatically (Abaqus 2024 expects the discontinued ifort)" }
}

# ---- 5. Build and test ABSS ----------------------------------------------------------------------------------------
if (-not $SkipBuild) {
    Step "Build and test ABSS"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "scripts\build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Build or tests failed." }
    Ok "built: $root\build\bin\abss.exe"
    $env:PATH = "$ucrt;$env:PATH"
    & (Join-Path $root "build\bin\abss-cli.exe") abaqus-info
}
Write-Host "`nSetup complete." -ForegroundColor Green
