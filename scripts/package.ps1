# Create a self-contained Windows distribution folder (and zip) of ABSS from an existing build.
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1 [-BuildDir build] [-OutDir dist]
# The result runs on a machine without MSYS2: abss.exe, abss-cli.exe, all dependent DLLs (resolved with ntldd/ldd),
# Qt plugins (windeployqt), examples and docs. Abaqus itself is NOT bundled (licensed vendor software).
param(
    [string]$BuildDir = "build",
    [string]$OutDir = "dist",
    [string]$Msys2Root = "$env:USERPROFILE\toolchains\msys64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "$BuildDir\bin"
$ucrt = Join-Path $Msys2Root "ucrt64\bin"
$out = Join-Path $root "$OutDir\ABSS"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
$exes = @("abss.exe", "abss-cli.exe") | Where-Object { Test-Path (Join-Path $bin $_) }
if (-not $exes) { throw "No executables in $bin; build first (scripts\build.ps1)" }
foreach ($e in $exes) { Copy-Item (Join-Path $bin $e) $out }

# Resolve DLL dependencies recursively from the PE import tables (objdump -p: static, never loads the DLL; MSYS2 ldd
# can hang on Qt DLLs). Only DLLs that exist in the UCRT64 bin directory are copied (system DLLs are left out).
$env:PATH = "$ucrt;$env:PATH"
$objdump = Join-Path $ucrt "objdump.exe"
$seen = @{}
$queue = New-Object System.Collections.Queue
foreach ($e in $exes) { $queue.Enqueue((Join-Path $out $e)) }
while ($queue.Count -gt 0) {
    $f = $queue.Dequeue()
    foreach ($l in (& $objdump -p $f 2>$null)) {
        if ($l -match "DLL Name:\s+(\S+\.dll)") {
            $name = $Matches[1]
            $src = Join-Path $ucrt $name
            if (-not $seen.ContainsKey($name.ToLower()) -and (Test-Path $src)) {
                $seen[$name.ToLower()] = $true
                Copy-Item $src $out -Force
                $queue.Enqueue((Join-Path $out $name))
            }
        }
    }
}
# Qt platform/style/image plugins.
$wdq = Join-Path $ucrt "windeployqt6.exe"
if (-not (Test-Path $wdq)) { $wdq = Join-Path $ucrt "windeployqt.exe" }
if ((Test-Path $wdq) -and (Test-Path (Join-Path $out "abss.exe"))) {
    & $wdq --no-translations --no-compiler-runtime --dir $out (Join-Path $out "abss.exe") | Out-Null
}
Copy-Item -Recurse (Join-Path $root "examples") (Join-Path $out "examples")
New-Item -ItemType Directory -Force (Join-Path $out "docs") | Out-Null
Copy-Item (Join-Path $root "README.md") $out -ErrorAction SilentlyContinue
foreach ($d in "SCIENCE.md", "KNOWN_LIMITATIONS.md", "VALIDATION_RESULTS.md", "ABAQUS_REQUIRED_TESTS.md", "ARCHITECTURE.md") {
    $p = Join-Path $root "docs\$d"
    if (Test-Path $p) { Copy-Item $p (Join-Path $out "docs") }
}
$zip = Join-Path $root "$OutDir\ABSS-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $out -DestinationPath $zip
Write-Output "Packaged $($seen.Count) DLLs into $out"
Write-Output "Archive: $zip"
