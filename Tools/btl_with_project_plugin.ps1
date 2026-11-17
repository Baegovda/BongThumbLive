# Run a background editor test with a built package installed as a *project* plugin (overrides the engine copy),
# then remove it again. Lets a build be tested in a real project without touching the engine plugin.
# Refuses if any UnrealEditor is running (the user's editor would pick the test plugin up).
# Usage: & btl_with_project_plugin.ps1 -Pkg <name> -Project <uproject> -Runs @( @{Args='...'; Log='...'}, ... )
param([Parameter(Mandatory)] [string]$Pkg, [Parameter(Mandatory)] [string]$Project, [Parameter(Mandatory)] [object[]]$Runs, [int]$TimeoutSec = 900)
$ErrorActionPreference = 'Stop'
if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) { throw 'an editor is running; not installing a test plugin into the project' }
$src = (Get-Item (Join-Path $env:TEMP "BongThumbLive_$Pkg")).FullName
$dst = Join-Path (Split-Path $Project) 'Plugins\BongThumbLive'
if (Test-Path $dst) { throw "$dst exists" }
New-Item -ItemType Directory -Force "$dst\Binaries\Win64" | Out-Null
Copy-Item "$src\BongThumbLive.uplugin" $dst
Copy-Item "$src\Binaries\Win64\*" "$dst\Binaries\Win64"
Copy-Item "$src\Source" $dst -Recurse
try {
	foreach ($r in $Runs) {
		& "$PSScriptRoot\btl_launch_bg.ps1" -Project $Project -EditorArgs $r.Args -LogCopy $r.Log -TimeoutSec $TimeoutSec | Select-String 'exit|TIMEOUT|peak'
	}
}
finally {
	for ($i = 0; $i -lt 60 -and (Get-Process UnrealEditor*, CrashReportClient* -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
	for ($i = 0; $i -lt 10 -and (Test-Path $dst); $i++) { Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue; Start-Sleep 1 }
	"project plugin copy removed: $(-not (Test-Path $dst))"
}
