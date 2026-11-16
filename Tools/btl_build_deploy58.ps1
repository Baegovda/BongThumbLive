# Build the repo plugin for UE 5.8 and deploy it into the engine copy.
# Backs up every replaced binary/source file as the next free .bakN. Refuses if a running editor has the plugin dll loaded.
# Usage: & btl_build_deploy58.ps1 -Pkg <name>   (package goes to %TEMP%\BongThumbLive_<name>)
param([Parameter(Mandatory)] [string]$Pkg, [switch]$NoDeploy, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot
$Out = Join-Path $env:TEMP "BongThumbLive_$Pkg"
$Log = Join-Path $env:TEMP "btl_${Pkg}_build.log"
if (-not $SkipBuild) {
if (Test-Path $Out) { throw "$Out exists" }
# Other workers on this PC also build with UE 5.8; UBT allows one instance (ConflictingInstance). Retry for up to 30 min.
for ($try = 1; $try -le 60; $try++) {
	& 'K:\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat' BuildPlugin -Plugin="$Repo\BongThumbLive.uplugin" -Package="$Out" -TargetPlatforms=Win64 -Rocket *> $Log
	if (-not (Select-String -Path $Log -Pattern 'ConflictingInstance' -Quiet)) { break }
	"UBT busy (another build), retry $try in 30 s"
	Remove-Item $Out -Recurse -Force -ErrorAction SilentlyContinue
	Start-Sleep -Seconds 30
}
$r = Select-String -Path $Log -Pattern 'warning C\d|error C\d|error LNK|BUILD SUCCESSFUL|BUILD FAILED' | ForEach-Object Line
$r
if (-not ($r -match 'BUILD SUCCESSFUL')) { throw "build failed, see $Log" }
}
if ($NoDeploy) { return }
foreach ($pp in Get-Process UnrealEditor* -ErrorAction SilentlyContinue) {
	if ($pp.Modules | Where-Object ModuleName -like '*BongThumbLive*') { throw "plugin dll in use by pid $($pp.Id)" }
}
$Out = (Get-Item $Out).FullName # %TEMP% may be a short 8.3 path; FullName below is long, so normalise first
$Dst = 'K:\Epic Games\UE_5.8\Engine\Plugins\Marketplace\BongThumbLive'
$changed = @('Binaries\Win64\UnrealEditor-BongThumbLive.dll', 'Binaries\Win64\UnrealEditor-BongThumbLive.pdb', 'Binaries\Win64\UnrealEditor.modules')
$changed += Get-ChildItem "$Out\Source" -Recurse -File | ForEach-Object { $_.FullName.Substring($Out.Length + 1) } |
	Where-Object { -not (Test-Path "$Dst\$_") -or (Get-FileHash "$Dst\$_").Hash -ne (Get-FileHash "$Out\$_").Hash }
foreach ($f in $changed) {
	if (Test-Path "$Dst\$f") { $n = 1; while (Test-Path "$Dst\$f.bak$n") { $n++ }; Copy-Item "$Dst\$f" "$Dst\$f.bak$n"; $b = ".bak$n" } else { $b = '(new)' }
	New-Item -ItemType Directory -Force (Split-Path "$Dst\$f") | Out-Null
	Copy-Item "$Out\$f" "$Dst\$f" -Force
	"deployed $f  backup $b"
}
