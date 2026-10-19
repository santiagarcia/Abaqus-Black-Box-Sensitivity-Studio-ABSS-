# ABSS installer helper: detect and (optionally) install the user-subroutine toolchain.
# Run by the ABSS setup program, which is already elevated (the user approved its administrator prompt), so the
# vendor installers below run silently without further prompts. Also usable on its own:
#   powershell -ExecutionPolicy Bypass -File toolchain.ps1            # report only
#   powershell -ExecutionPolicy Bypass -File toolchain.ps1 -Install   # install what is missing (needs admin)
param([switch]$Install)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$IntelUrl = "https://registrationcenter-download.intel.com/akdlm/IRC_NAS/77bf3a15-a766-4619-91b4-1011badc4a36/intel-oneapi-hpc-toolkit-2025.1.3.7_offline.exe"
$IntelSha = "ead9cf59a11e411ebd670e51ca7fa27f8679e158fc4d70b85b0f0f361ada56f6"
$VsUrl = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
$Cache = Join-Path $env:ProgramData "ABSS\installers"

function Find-VS {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($p) { return $p } else { return $null }
}
function Find-Fortran {
    foreach ($root in @($env:ONEAPI_ROOT, "${env:ProgramFiles(x86)}\Intel\oneAPI", "$env:ProgramFiles\Intel\oneAPI")) {
        if ($root -and (Test-Path (Join-Path $root "compiler"))) {
            $f = Get-ChildItem (Join-Path $root "compiler") -Recurse -Include ifort.exe, ifx.exe -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($f) { return $f.FullName }
        }
    }
    return $null
}
function Find-Abaqus {
    foreach ($c in @($env:ABSS_ABAQUS_CMD, "C:\SIMULIA\Commands\abaqus.bat", "$env:ProgramFiles\SIMULIA\Commands\abaqus.bat")) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}
function Get-File($url, $dest) {
    Write-Output "  Downloading $url"
    Write-Output "  (large download - this can take several minutes)"
    try { Start-BitsTransfer -Source $url -Destination $dest -ErrorAction Stop }
    catch { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $dest }
}

$vs = Find-VS
$ft = Find-Fortran
$abq = Find-Abaqus
Write-Output ("Abaqus:                     " + $(if ($abq) { "found ($abq)" } else { "not found - needed only for new analyses; the demos work without it" }))
if ($abq) {
    $info = (cmd /c "`"$abq`" information=release" 2>$null) -join " "
    if ($info -match "located in the directory\s+(\S+)") {
        $dev = Test-Path (Join-Path $Matches[1] "win_b64\SMA\site\aba_param.inc")
        Write-Output ("Abaqus user-subroutine files: " + $(if ($dev) { "installed (ABSS uses 'abaqus make')" } else { "not installed - ABSS builds UMAT/UEL libraries directly (no change to Abaqus needed)" }))
    }
}
Write-Output ("Microsoft C++ Build Tools:  " + $(if ($vs) { "found ($vs)" } else { "not installed" }))
Write-Output ("Intel Fortran:              " + $(if ($ft) { "found ($ft)" } else { "not installed" }))
if (-not $Install) { exit 0 }

New-Item -ItemType Directory -Force $Cache | Out-Null
$failed = $false
if (-not $vs) {
    Write-Output "Installing Microsoft C++ Build Tools (Visual Studio 2022)..."
    $exe = Join-Path $Cache "vs_BuildTools.exe"
    Get-File $VsUrl $exe
    $sig = Get-AuthenticodeSignature $exe
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Microsoft") {
        Write-Output "  ERROR: the Visual Studio bootstrapper is not validly signed by Microsoft; not running it."
        $failed = $true
    } else {
        $p = Start-Process -FilePath $exe -ArgumentList "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" -PassThru -Wait
        # Success is decided by what is installed afterwards; vendor exit codes also signal warnings (e.g. 3010 = restart).
        if (Find-VS) { Write-Output "  Installed$(if ($p.ExitCode -ne 0) { " (installer reported code $($p.ExitCode); a restart may be recommended)" })." }
        else { Write-Output "  ERROR: Visual Studio Build Tools are still missing (installer exit code $($p.ExitCode))."; $failed = $true }
    }
}
if (-not $ft) {
    Write-Output "Installing Intel oneAPI HPC Toolkit 2025.1 (Fortran compiler, ~2.8 GB download)..."
    $exe = Join-Path $Cache (Split-Path $IntelUrl -Leaf)
    if (-not (Test-Path $exe) -or (Get-FileHash $exe -Algorithm SHA256).Hash -ine $IntelSha) { Get-File $IntelUrl $exe }
    if ((Get-FileHash $exe -Algorithm SHA256).Hash -ine $IntelSha) {
        Remove-Item $exe -Force
        Write-Output "  ERROR: the Intel installer's SHA-256 does not match the published value; download deleted."
        $failed = $true
    } else {
        Write-Output "  Verified SHA-256. Installing (10-20 minutes)..."
        $p = Start-Process -FilePath $exe -ArgumentList "-s -a --silent --eula accept" -PassThru -Wait
        if (Find-Fortran) { Write-Output "  Installed$(if ($p.ExitCode -ne 0) { " (installer reported code $($p.ExitCode) - typically an optional integration or restart notice; the compiler is present)" })." }
        else { Write-Output "  ERROR: Intel Fortran is still missing (installer exit code $($p.ExitCode))."; $failed = $true }
    }
}
$vs = Find-VS
$ft = Find-Fortran
Write-Output ("Result: Microsoft C++ Build Tools " + $(if ($vs) { "OK" } else { "MISSING" }) + ", Intel Fortran " + $(if ($ft) { "OK" } else { "MISSING" }))
if ($ft -and $ft -like "*ifx.exe") { Write-Output "Note: ifx found; ABSS configures Abaqus 2024 to use it automatically (ifort is no longer distributed by Intel)." }
if ($failed -or -not $vs -or -not $ft) { exit 1 }
exit 0
