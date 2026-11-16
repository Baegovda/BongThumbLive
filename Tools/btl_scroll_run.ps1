param([Parameter(Mandatory)][string]$Tag)
$T = $env:TEMP
Set-Content "$T\btl_scroll_tag.txt" $Tag
$al = "`"K:\Unreal Projects\UE_VFX_58\UE_VFX_58.uproject`" -AutoDeclinePackageRecovery " +
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False " +
      "-ExecCmds=`"py $T\btl_scroll_test.py`""
$t0=Get-Date
$p = Start-Process "K:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" -ArgumentList $al -PassThru
$p.WaitForExit(600000) | Out-Null
"exited=$($p.HasExited) code=$($p.ExitCode) start=$t0 end=$(Get-Date)"
Copy-Item "K:\Unreal Projects\UE_VFX_58\Saved\Logs\UE_VFX_58.log" "$T\btl_scroll_$Tag.log" -Force
Select-String -Path "$T\btl_scroll_$Tag.log" -Pattern "BTLD|memory cache:|live:|Error:|Fatal|LogExit" | % { $_.Line } | Select -Last 60
