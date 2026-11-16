param([Parameter(Mandatory)][string]$Tag)
$T = $env:TEMP
Set-Content "$T\btl_sm2_tag.txt" $Tag
$al = "`"K:\Unreal Projects\UE_VFX_58\UE_VFX_58.uproject`" -AutoDeclinePackageRecovery " +
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False " +
      "-ExecCmds=`"py $T\btl_sm2_test.py`""
$p = Start-Process "K:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" -ArgumentList $al -PassThru
$p.WaitForExit(600000) | Out-Null
"exited=$($p.HasExited) code=$($p.ExitCode) $(Get-Date)"
Copy-Item "K:\Unreal Projects\UE_VFX_58\Saved\Logs\UE_VFX_58.log" "$T\btl_sm2_$Tag.log" -Force
Select-String -Path "$T\btl_sm2_$Tag.log" -Pattern "BTLD|BongThumbLive.Status|gave up|Error:|Fatal" | % { $_.Line } | Select -Last 80
