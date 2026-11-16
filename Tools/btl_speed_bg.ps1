# Cold-cache speed run in the background (side monitor) via btl_launch_bg.ps1.
# Default project is the disposable C:\workspace\BTLTest58 (copy of /Game/Props/PrecisionSniperRifle).
# Usage: & btl_speed_bg.ps1 -Tag X [-Cvars "BongThumbLive.TightFraming 0;..."] [-Project <uproject>] [-Folder /Game/...]
param(
	[Parameter(Mandatory)] [string]$Tag,
	[string]$Cvars = '',
	[string]$Project = 'C:\workspace\BTLTest58\BTLTest58.uproject',
	[string]$Folder = '/Game/Props/PrecisionSniperRifle/Character/Animations',
	[switch]$Warm # keep the plugin disk cache (folder-click latency with cached frames)
)
$ErrorActionPreference = 'Stop'
$T = $env:TEMP
$Tools = $PSScriptRoot
Copy-Item "$Tools\btl_speed_test.py", "$Tools\btl_speed_parse.py" $T -Force
Set-Content "$T\btl_speed_cfg.txt" "$Tag | $Folder | $Cvars"
$ProjDir = Split-Path $Project
$Cache = Join-Path $ProjDir 'Saved\BongThumbLive\Cache'
if (-not $Warm -and (Test-Path $Cache)) { Get-ChildItem $Cache -File | Remove-Item -Force }
"cache files after clear: $((Get-ChildItem $Cache -File -EA 0).Count)"
$ea = '-AutoDeclinePackageRecovery -ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False ' +
	"-ExecCmds=`"py $T\btl_speed_test.py`""
& "$Tools\btl_launch_bg.ps1" -Project $Project -EditorArgs $ea -LogCopy "$T\btl_speed_$Tag.log" -TimeoutSec 600
& 'C:\Program Files\Python310\python.exe' -I "$T\btl_speed_parse.py" "$T\btl_speed_$Tag.log"
