# Configure, build and test ABSS on Windows with the MSYS2 UCRT64 toolchain.
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-NoGui] [-NoTests] [-BuildDir build]
param(
    [string]$BuildDir = "build",
    [string]$Msys2Root = "$env:USERPROFILE\toolchains\msys64",
    [switch]$NoGui,
    [switch]$NoTests,
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$ucrt = Join-Path $Msys2Root "ucrt64\bin"
if (-not (Test-Path (Join-Path $ucrt "g++.exe"))) { throw "MSYS2 UCRT64 g++ not found under $ucrt (see README: Installation)" }
$env:PATH = "$ucrt;$env:PATH"
$gui = if ($NoGui) { "OFF" } else { "ON" }
& cmake -S $root -B (Join-Path $root $BuildDir) -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_TOOLCHAIN_FILE=$root/cmake/msys2-ucrt64.cmake" "-DABSS_MSYS2_ROOT=$Msys2Root" "-DABSS_BUILD_GUI=$gui"
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
& cmake --build (Join-Path $root $BuildDir) -j 8
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
if (-not $NoTests) {
    & ctest --test-dir (Join-Path $root $BuildDir) --output-on-failure -LE abaqus
    if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
}
