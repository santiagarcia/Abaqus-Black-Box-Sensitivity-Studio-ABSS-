# Build the ABSS verification package: re-run every real-Abaqus verification with the current build (full logs and
# .abss result files that open in the GUI without Abaqus), capture GUI screenshots, and bundle them with the example
# models, the docs and the walkthrough.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make_verification_package.ps1 [-Out dist\ABSS-Verification] [-Parallel 4] [-SkipRuns]
#
# Needs: a build (scripts\build.ps1), Abaqus with a licence, and the user-subroutine compilers for the UMAT/UEL/bridge
# cases. Proprietary third-party material packages are never included.
param(
    [string]$Out = "dist\ABSS-Verification",
    [int]$Parallel = 4,
    [switch]$SkipRuns,
    [string]$Msys2Root = "$env:USERPROFILE\toolchains\msys64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root $Out
$bin = Join-Path $root "build\bin"
$env:PATH = "$(Join-Path $Msys2Root 'ucrt64\bin');$env:PATH"
New-Item -ItemType Directory -Force (Join-Path $out "results"), (Join-Path $out "screenshots") | Out-Null

# The bridge test material (a Windows DLL with a deformation-gradient interface, like a vendor library).
$bridgeLib = Join-Path $out "examples\bridge_material"
New-Item -ItemType Directory -Force $bridgeLib | Out-Null
& gcc -shared -O2 -static-libgcc -o (Join-Path $bridgeLib "nh_regular.dll") (Join-Path $root "tests\abaqus\bridge_material\nh_regular.c")
if ($LASTEXITCODE -ne 0) { throw "could not build the bridge test material" }
Copy-Item (Join-Path $root "tests\abaqus\bridge_material\nh_regular.*") $bridgeLib

$m = Join-Path $root "examples\models"
$s = Join-Path $root "examples\subroutines"
# name, model, parameters, extra options
$cases = @(
    @("01_truss_order2", "$m\truss.inp", "mat:STEEL:elastic:E,load:LOAD:cload:TIP:1,geom:TIP:X", @("--order", "2")),
    @("02_j2_plasticity", "$m\j2_plastic.inp", "mat:STEEL:elastic:E,mat:STEEL:plastic:sigma_y:1,bc:LOAD:RIGHT:1-1", @("--jobs", "6")),
    @("03_part_instance_block", "$m\c3d8_block.inp", "mat:ALU:elastic:E,load:PRESS:cload:BLOCK-1.27:3,geom:BLOCK-1.TOP:Z", @()),
    @("04_two_part_assembly_nlgeom", "$m\two_part_assembly.inp", "mat:STEEL:elastic:E,mat:ALU:elastic:E,load:PRESS:cload:TOP_B:2", @()),
    @("05_include_files", "$m\include_demo\main.inp", "mat:ALU:elastic:E", @()),
    @("06_umat_source", "$m\umat_props.inp", "umat:UMAT_MAT:props:1,umat:UMAT_MAT:props:2", @("--user", "$s\umat_elastic.f")),
    @("07_uel_source", "$m\uel_props.inp", "uel:UTRUSS:props:1,uel:UTRUSS:props:2", @("--user", "$s\uel_truss.f")),
    @("08_material_bridge", "$m\bridge_nh_block.inp", "umat:NH:props:1,umat:NH:props:2", @("--user", (Join-Path $bridgeLib "nh_regular.dll"))),
    @("09_constraints_tie_coupling", "$m\constraints_tie_coupling.inp", "mat:SOFT:elastic:E,mat:STIFF:elastic:E,load:LOAD:cload:200:1", @()),
    @("10_shell_large_rotation", "$m\shell_large_rotation.inp", "mat:M:elastic:E,load:BEND:cload:TIP:5", @()),
    @("11_dynamic_hht", "$m\dynamic_cantilever.inp", "mat:M:elastic:E,mat:M:density:density", @()),
    @("12_bc_release_two_steps", "$m\bc_release_two_steps.inp", "mat:M:elastic:E,load:HOLD:cload:27:1", @()),
    @("13_thermal_smooth_step", "$m\thermal_smooth_step.inp", "mat:M:elastic:E,mat:M:expansion:alpha", @()),
    @("14_visco_creep", "$m\visco_creep.inp", "mat:M:elastic:E,load:CREEP:cload:27:1", @()),
    @("15_follower_pressure_nlgeom", "$m\follower_pressure_nlgeom.inp", "mat:M:elastic:E,load:PUSH:dsload:TOPF:P", @()),
    @("16_contact_blocks", "$m\contact_blocks.inp", "mat:SOFT:elastic:E,mat:STIFF:elastic:E", @())
)

if (-not $SkipRuns) {
    $running = @()
    foreach ($c in $cases) {
        while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Parallel) { Start-Sleep 5 }
        $name = $c[0]
        $log = Join-Path $out "results\$name.log"
        $argList = @("run", "`"$($c[1])`"", "--params", $c[2], "--order", "1", "--jobs", "3", "--validate",
                     "--scratch", "`"$env:TEMP\abss_pkg_$name`"", "--out", "`"$(Join-Path $out "results\$name.abss")`"") + $c[3]
        Write-Output "Running $name"
        $running += Start-Process -FilePath (Join-Path $bin "abss-cli.exe") -ArgumentList $argList -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    }
    $running | ForEach-Object { $_.WaitForExit() }
}
# Summary table (status line of every derivative), from the logs
$logTime = (Get-ChildItem (Join-Path $out "results\*.log") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
if ($logTime) {
    $sum = "# Verification summary`n`nRuns finished $($logTime.ToString('yyyy-MM-dd HH:mm')) on real Abaqus 2024 with the ABSS build in this package. Tolerance: 1e-3 relative (order 1), 1e-2 (order 2).`n`n| Case | Derivative | Status | Max rel. error vs full Abaqus reruns |`n|---|---|---|---|`n"
    foreach ($c in $cases) {
        $log = Join-Path $out "results\$($c[0]).log"
        $err = Get-Content "$log.err" -ErrorAction SilentlyContinue
        $lines = Get-Content $log -Encoding UTF8 -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\s+d.+?/d.+?: (Validated|Failed|Unverified|Experimental)' }
        if (-not $lines) { $sum += "| $($c[0]) | - | ERROR | $(($err | Select-Object -Last 1) -replace '\|', '/') |`n"; continue }
        foreach ($l in $lines) {
            if ($l -match '^\s+(d.+?): (\w+), rerun-FD max rel\. error ([0-9.eE+-]+)') { $sum += "| $($c[0]) | $($Matches[1]) | $($Matches[2]) | $($Matches[3]) |`n" }
            elseif ($l -match '^\s+(d.+?): (\w+)') { $sum += "| $($c[0]) | $($Matches[1]) | $($Matches[2]) | - |`n" }
        }
    }
    Set-Content (Join-Path $out "results\SUMMARY.md") $sum -Encoding utf8
}

# GUI screenshots (unattended automation of the real application).
$shots = Join-Path $out "screenshots"
$gui = Join-Path $bin "abss.exe"
function Shot($argList) { $p = Start-Process -FilePath $gui -ArgumentList $argList -PassThru -Wait; if ($p.ExitCode -ne 0) { Write-Warning "screenshot step exited with $($p.ExitCode): $argList" } }
Shot @("--page", "start", "--size", "1500x950", "--screenshot", "`"$shots\01_start.png`"", "--quit")
Shot @("`"$m\c3d8_block.inp`"", "--page", "parameters", "--size", "1500x950", "--screenshot", "`"$shots\02_parameters.png`"", "--quit")
Shot @("`"$m\bridge_nh_block.inp`"", "--page", "setup", "--size", "1500x950", "--screenshot", "`"$shots\03_setup_user_material.png`"", "--quit")
$res = Join-Path $out "results"
if (Test-Path "$res\09_constraints_tie_coupling.abss") {
    Shot @("`"$res\09_constraints_tie_coupling.abss`"", "--page", "results", "--output", "S_Mises", "--result-order", "1", "--pick", "2", "--size", "1500x950",
           "--screenshot", "`"$shots\04_results_constraints.png`"", "--chart-shot", "`"$shots\05_history_constraints.png`"", "--quit")
}
if (Test-Path "$res\11_dynamic_hht.abss") {
    Shot @("`"$res\11_dynamic_hht.abss`"", "--page", "results", "--output", "U3", "--result-order", "1", "--pick", "1", "--size", "1500x950",
           "--screenshot", "`"$shots\06_results_dynamic.png`"", "--chart-shot", "`"$shots\07_history_dynamic.png`"", "--quit")
}
if (Test-Path "$res\10_shell_large_rotation.abss") {
    Shot @("`"$res\10_shell_large_rotation.abss`"", "--page", "results", "--output", "U3", "--result-order", "1", "--size", "1500x950",
           "--screenshot", "`"$shots\08_results_shell.png`"", "--quit")
}
Shot @("--demo", "plate", "--run", "--pick", "2", "--size", "1500x950", "--screenshot", "`"$shots\09_demo_plate.png`"", "--quit")

# Examples, docs, walkthrough.
Copy-Item -Recurse -Force (Join-Path $root "examples\models") (Join-Path $out "examples\models")
Copy-Item -Recurse -Force (Join-Path $root "examples\subroutines") (Join-Path $out "examples\subroutines")
New-Item -ItemType Directory -Force (Join-Path $out "docs") | Out-Null
foreach ($d in "SCIENCE.md", "KNOWN_LIMITATIONS.md", "VALIDATION_RESULTS.md", "ABAQUS_REQUIRED_TESTS.md", "ARCHITECTURE.md") {
    Copy-Item (Join-Path $root "docs\$d") (Join-Path $out "docs")
}
Copy-Item (Join-Path $root "README.md") $out
# Walkthrough with the measured results table (from the logs of this package's runs).
$html = Get-Content (Join-Path $root "packaging\WALKTHROUGH.html") -Raw -Encoding UTF8
$tbl = "<table><tr><th>Case</th><th>Derivative</th><th>Status</th><th>Max rel. error vs full reruns</th></tr>"
foreach ($c in $cases) {
    $log = Join-Path $out "results\$($c[0]).log"
    foreach ($l in (Get-Content $log -Encoding UTF8 -ErrorAction SilentlyContinue)) {
        if ($l -match '^\s+(d.+?): (\w+), rerun-FD max rel\. error ([0-9.eE+-]+)') {
            $cls = @{ Validated = "ok"; Experimental = "warn"; Failed = "bad"; Unverified = "warn" }[$Matches[2]]
            $tbl += "<tr><td>$($c[0])</td><td>$([System.Net.WebUtility]::HtmlEncode($Matches[1]))</td><td class=`"$cls`">$($Matches[2])</td><td>$('{0:G3}' -f [double]$Matches[3])</td></tr>"
        }
    }
}
$tbl += "</table>"
Set-Content (Join-Path $out "WALKTHROUGH.html") ($html -replace '<!--RESULTS_TABLE-->', $tbl) -Encoding utf8
$zip = "$out.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$out\*" -DestinationPath $zip
Write-Output "Package: $out"
Write-Output "Archive: $zip"

