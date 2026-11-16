# Recreate the disposable UE 5.8 regression project (C:\workspace\BTLTest58 by default).
# Copies /Game/Props/PrecisionSniperRifle from UE_VFX_58 (read-only source) and writes the BTL_DISPOSABLE_TEST_PROJECT
# marker that Tools/Tests/run_editor_regressions.py requires. The first launch compiles shaders (several minutes).
param(
	[string]$Dir = 'C:\workspace\BTLTest58',
	[string]$Source = 'K:\Unreal Projects\UE_VFX_58\Content\Props\PrecisionSniperRifle'
)
$ErrorActionPreference = 'Stop'
if (Test-Path $Dir) { throw "$Dir exists" }
New-Item -ItemType Directory -Force "$Dir\Content\Props", "$Dir\Config" | Out-Null
@'
{
	"FileVersion": 3,
	"EngineAssociation": "5.8",
	"Category": "",
	"Description": "Disposable BongThumbLive regression project",
	"Plugins": [
		{ "Name": "BongThumbLive", "Enabled": true },
		{ "Name": "PythonScriptPlugin", "Enabled": true },
		{ "Name": "EditorScriptingUtilities", "Enabled": true }
	]
}
'@ | Set-Content "$Dir\BTLTest58.uproject" -Encoding utf8
'' | Set-Content "$Dir\Config\DefaultEngine.ini"
New-Item -ItemType File "$Dir\BTL_DISPOSABLE_TEST_PROJECT" | Out-Null
robocopy $Source "$Dir\Content\Props\PrecisionSniperRifle" /E /NFL /NDL /NJH /NP /MT:16 | Select-Object -Last 3
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
"created $Dir"
