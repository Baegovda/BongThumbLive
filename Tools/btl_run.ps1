# Usage: pwsh -File btl_run.ps1 -Run <name>   (uses Temp\btl_bench_cfg.json; sets "run" to <name>)
# Launches the editor with the benchmark script, background throttling off for this session only, records memory,
# copies the log to Temp\btl_<name>.log and prints the parsed summary.
param([Parameter(Mandatory)][string]$Run, [string]$Extra = "")
$T = $env:TEMP
$cfg = Get-Content "$T\btl_bench_cfg.json" -Raw | ConvertFrom-Json
$cfg.run = $Run
$cfg | ConvertTo-Json -Depth 5 | Set-Content "$T\btl_bench_cfg.json" -Encoding utf8
$repo = "$T\BongThumbLive_repo\Tools"
$wd = Start-Process pwsh -ArgumentList "-NoProfile -File `"$repo\btl_watchdog.ps1`" -Out `"$T\btl_mem_$Run.csv`"" -WindowStyle Hidden -PassThru
$al = "`"K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject`" -AutoDeclinePackageRecovery " +
        "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False " +
        "-ExecCmds=`"py $T\btl_bench.py`" $Extra"
$p = Start-Process "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" -ArgumentList $al -PassThru
$p.WaitForExit(900000) | Out-Null
Stop-Process -Id $wd.Id -ErrorAction SilentlyContinue
"exited=$($p.HasExited) code=$($p.ExitCode) $(Get-Date)"
Copy-Item "K:\Unreal Projects\UE_VFX_57\Saved\Logs\UE_VFX_57.log" "$T\btl_$Run.log" -Force
& "C:\Program Files\Python310\python.exe" "$T\btl_bench_parse.py" "$T\btl_$Run.log"
