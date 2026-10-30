param([Parameter(Mandatory)][string]$Tag, [string]$Folder = "/Game/Props/PrecisionSniperRifle/Character/Animations", [string]$Cvars = "")
$T = $env:TEMP
Set-Content "$T\btl_speed_cfg.txt" "$Tag | $Folder | $Cvars"
$C='K:\Unreal Projects\UE_VFX_57\Saved\BongThumbLive\Cache'
if (Get-CimInstance Win32_Process -Filter "Name like 'UnrealEditor%'" | ? { $_.CommandLine -like '*UE_VFX_57*' }) { throw "UE_VFX_57 editor running" }
if (Test-Path $C) { Get-ChildItem $C -File | Remove-Item -Force }
"cache files after clear: $((Get-ChildItem $C -File -EA 0).Count)"
$al = "`"K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject`" -AutoDeclinePackageRecovery " +
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False " +
      "-ExecCmds=`"py $T\btl_speed_test.py`""
$p = Start-Process "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" -ArgumentList $al -PassThru
$peak=0; $sw=[Diagnostics.Stopwatch]::StartNew()
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt 600) { try { $p.Refresh(); $peak=[Math]::Max($peak,$p.PrivateMemorySize64) } catch {}; Start-Sleep 1 }
"exited=$($p.HasExited) peakPrivateGB=$([Math]::Round($peak/1GB,2))"
Copy-Item "K:\Unreal Projects\UE_VFX_57\Saved\Logs\UE_VFX_57.log" "$T\btl_speed_$Tag.log" -Force
& "C:\Program Files\Python310\python.exe" "$T\btl_speed_parse.py" "$T\btl_speed_$Tag.log" $([Math]::Round($peak/1GB,2))
