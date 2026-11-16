# Wait until no running editor has the BongThumbLive dll loaded (the user may be working), then deploy a built package
# into the UE 5.8 engine copy (btl_build_deploy58.ps1 -SkipBuild) and set the engine .uplugin version.
# Usage: & btl_deploy_when_free.ps1 -Pkg <name> -VersionName 2.2.2 -Version 6 [-MaxHours 12]
param([Parameter(Mandatory)] [string]$Pkg, [Parameter(Mandatory)] [string]$VersionName, [Parameter(Mandatory)] [int]$Version, [double]$MaxHours = 12)
$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddHours($MaxHours)
function Test-InUse {
	foreach ($pp in Get-Process UnrealEditor* -ErrorAction SilentlyContinue) {
		try { if ($pp.Modules | Where-Object ModuleName -like '*BongThumbLive*') { return $pp.Id } } catch {}
	}
	return $null
}
while ($true) {
	$busy = Test-InUse
	if (-not $busy) { break }
	if ((Get-Date) -gt $deadline) { throw "still in use by pid $busy after $MaxHours h; not deployed" }
	Start-Sleep -Seconds 30
}
"$(Get-Date -Format 'HH:mm:ss') plugin dll free, deploying $Pkg"
& "$PSScriptRoot\btl_build_deploy58.ps1" -Pkg $Pkg -SkipBuild
$u = 'K:\Epic Games\UE_5.8\Engine\Plugins\Marketplace\BongThumbLive\BongThumbLive.uplugin'
$n = 1; while (Test-Path "$u.bak$n") { $n++ }; Copy-Item $u "$u.bak$n"
$t = [IO.File]::ReadAllText($u)
$t = [regex]::Replace($t, '"Version": \d+,', "`"Version`": $Version,")
$t = [regex]::Replace($t, '"VersionName": "[^"]*",', "`"VersionName`": `"$VersionName`",")
[IO.File]::WriteAllText($u, $t)
"engine .uplugin -> $VersionName (backup .bak$n)"
