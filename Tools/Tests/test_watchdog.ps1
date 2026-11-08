param(
  [Parameter(Mandatory)][int]$EditorProcessId,
  [Parameter(Mandatory)][string]$ProjectPath,
  [Parameter(Mandatory)][string]$EditorExecutable,
  [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$watchdog=Join-Path $PSScriptRoot '../btl_watchdog.ps1'
$editor=Get-Process -Id $EditorProcessId
$ticks=$editor.StartTime.ToUniversalTime().Ticks
$outFile=Join-Path $OutputDirectory 'watchdog-unused.csv'
$results=@()
function Check-Rejection([string]$Name,[hashtable]$Overrides) {
  $arguments=@{ProcessId=$EditorProcessId;ExpectedStartTimeTicks=$ticks;ProjectPath=$ProjectPath;EditorExecutable=$EditorExecutable;Out=$outFile;ValidateOnly=$true}
  foreach($key in $Overrides.Keys){$arguments[$key]=$Overrides[$key]}
  $rejected=$false
  try { & $watchdog @arguments | Out-Null } catch { $rejected=$true }
  if(-not $rejected){throw "Expected rejection: $Name"}
  if(-not (Get-Process -Id $EditorProcessId -ErrorAction SilentlyContinue)){throw 'Safety test unexpectedly stopped the editor'}
  return @{name=$Name;passed=$true}
}
& $watchdog -ProcessId $EditorProcessId -ExpectedStartTimeTicks $ticks -ProjectPath $ProjectPath -EditorExecutable $EditorExecutable -Out $outFile -ValidateOnly | Out-Null
$results+=@{name='matching editor accepted';passed=$true}
$results+=Check-Rejection 'PID lifetime mismatch' @{ExpectedStartTimeTicks=($ticks-1)}
$results+=Check-Rejection 'wrong project' @{ProjectPath=(Join-Path $OutputDirectory 'Other.uproject')}
$results+=Check-Rejection 'wrong executable' @{EditorExecutable=(Join-Path $OutputDirectory 'UnrealEditor.exe')}
$helper=Start-Process pwsh -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 60"' -WindowStyle Hidden -PassThru
try {
  $results+=Check-Rejection 'unrelated process cannot be targeted' @{ProcessId=$helper.Id;ExpectedStartTimeTicks=$helper.StartTime.ToUniversalTime().Ticks}
  if(-not (Get-Process -Id $helper.Id -ErrorAction SilentlyContinue)){throw 'Unrelated process was stopped'}
} finally {Stop-Process -Id $helper.Id -ErrorAction SilentlyContinue}
$results|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $OutputDirectory 'watchdog-tests.json')
$results|ConvertTo-Json
