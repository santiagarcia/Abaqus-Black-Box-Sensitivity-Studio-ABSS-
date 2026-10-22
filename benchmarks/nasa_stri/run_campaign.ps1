# Runs campaign stages in order with frozen binaries. Resumable: finished runs (JSON present) are skipped.
#   powershell -File benchmarks\nasa_stri\run_campaign.ps1 -Bin <snapshot dir> [-BinV1 <dir>] [-Stages "A_mesh_j8,B_mesh_j8"]
#   [-WaitForPids "123,456"]   (start only after these processes have exited, so runs are not contended)
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [string]$BinV1 = "",
    [string]$Stages = "A_mesh_j8,A_params_j8,B_mesh_j8,before_after,A_mesh_j1,A_inc_j8",
    [string]$Results = "benchmarks\nasa_stri\results",
    [string]$WaitForPids = ""
)
$ErrorActionPreference = "Continue"
$env:PATH = "C:\Users\vfn333\toolchains\msys64\ucrt64\bin;" + $env:PATH
$env:ABSS_BENCH_BIN = $Bin
$env:ABSS_BENCH_BIN_V1 = $BinV1
$py = "C:\Users\vfn333\toolchains\msys64\ucrt64\bin\python3.exe"
foreach ($p in ($WaitForPids -split "[,; ]" | Where-Object { $_ })) {
    $proc = Get-Process -Id ([int]$p) -ErrorAction SilentlyContinue
    if ($proc) { Write-Output "waiting for process $p"; $proc.WaitForExit() }
}
foreach ($s in ($Stages -split ",")) {
    Write-Output "=== stage $s  $(Get-Date -Format s)"
    & $py benchmarks\nasa_stri\campaign.py "benchmarks\nasa_stri\plans\$s.json" "$Results\$s"
    & $py benchmarks\nasa_stri\compare_all.py "$Bin\abss-cli.exe" "$Results\$s"
    & $py -I benchmarks\nasa_stri\accuracy.py --all "$Results\$s"
}
Write-Output "=== campaign finished $(Get-Date -Format s)"
