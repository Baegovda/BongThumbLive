param([Parameter(Mandatory)][string]$Tag, [string]$Folder = "/Game/SM2_Work")
$T = $env:TEMP
Set-Content "$T\btl_spin_tag.txt" $Tag; Set-Content "$T\btl_spin_folder.txt" $Folder
$C='K:\Unreal Projects\UE_VFX_58\Saved\BongThumbLive\Cache'
if (Get-CimInstance Win32_Process -Filter "Name like 'UnrealEditor%'" | ? { $_.CommandLine -like '*UE_VFX_58*' }) { throw "UE_VFX_58 editor running" }
if (Test-Path $C) { Get-ChildItem $C -File | Remove-Item -Force }
"cache files after clear: $((Get-ChildItem $C -File -EA 0).Count)"
$al = "`"K:\Unreal Projects\UE_VFX_58\UE_VFX_58.uproject`" -AutoDeclinePackageRecovery " +
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False " +
      "-ExecCmds=`"py $T\btl_spin_test.py`""
$t0=Get-Date
$p = Start-Process "K:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" -ArgumentList $al -PassThru
$p.WaitForExit(900000) | Out-Null
"exited=$($p.HasExited) code=$($p.ExitCode) start=$t0 end=$(Get-Date)"
Copy-Item "K:\Unreal Projects\UE_VFX_58\Saved\Logs\UE_VFX_58.log" "$T\btl_spin_$Tag.log" -Force
Select-String -Path "$T\btl_spin_$Tag.log" -Pattern "BTLD|spinner:|pushTargetSizes|Error:|Fatal" | % { $_.Line } | Select -Last 80
