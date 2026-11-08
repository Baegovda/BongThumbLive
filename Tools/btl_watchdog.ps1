param(
  [Parameter(Mandatory)][int]$ProcessId,
  [Parameter(Mandatory)][long]$ExpectedStartTimeTicks,
  [Parameter(Mandatory)][string]$ProjectPath,
  [Parameter(Mandatory)][string]$EditorExecutable,
  [Parameter(Mandatory)][string]$Out,
  [double]$LimitGB=45,
  [double]$MinFreeGB=12,
  [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
$projectFull = [IO.Path]::GetFullPath($ProjectPath).Replace('/','\')
$editorFull = [IO.Path]::GetFullPath($EditorExecutable).Replace('/','\')
if ([IO.Path]::GetFileName($editorFull) -ne 'UnrealEditor.exe') { throw 'Expected UnrealEditor.exe' }
function Get-VerifiedEditor {
  $candidate = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  if (-not $candidate) { return $null }
  if ($candidate.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTimeTicks) { throw 'PID was reused: start time mismatch' }
  $native = Get-CimInstance Win32_Process -Filter "ProcessId=$ProcessId"
  if (-not $native -or $native.ExecutablePath -ne $editorFull) { throw 'Editor executable mismatch' }
  $command = ([string]$native.CommandLine).Replace('/','\')
  $pattern = '(?i)(?:^|\s|")' + [regex]::Escape($projectFull) + '(?:"|\s|$)'
  if ($command -notmatch $pattern) { throw 'Project command line mismatch' }
  return $candidate
}
$target = Get-VerifiedEditor
if (-not $target) { throw 'Target editor is no longer running' }
if ($ValidateOnly) { "Verified PID=$ProcessId project=$projectFull"; exit 0 }
'time,ws_gb,private_gb,free_gb,gpu_ded_gb,gpu_shared_gb' | Set-Content -LiteralPath $Out
for ($i=0; $i -lt 2000; $i++) {
  $target = Get-VerifiedEditor
  if (-not $target) { break }
  $free = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1MB
  $ded=0; $shared=0
  try {
    $counter=Get-Counter "\GPU Process Memory(pid_$ProcessId*)\Dedicated Usage","\GPU Process Memory(pid_$ProcessId*)\Shared Usage" -ErrorAction Stop
    foreach ($sample in $counter.CounterSamples) {
      if ($sample.Path -like '*dedicated*') { $ded += $sample.CookedValue } else { $shared += $sample.CookedValue }
    }
  } catch {} # Optional GPU counters; identity checks above must never be swallowed.
  $line = [string]::Format([cultureinfo]::InvariantCulture,'{0},{1:N2},{2:N2},{3:N2},{4:N2},{5:N2}',
    (Get-Date -Format HH:mm:ss),($target.WorkingSet64/1GB),($target.PrivateMemorySize64/1GB),$free,($ded/1GB),($shared/1GB))
  Add-Content -LiteralPath $Out -Value $line
  if ($target.PrivateMemorySize64/1GB -gt $LimitGB -or $free -lt $MinFreeGB) {
    $target = Get-VerifiedEditor
    if ($target) {
      Add-Content -LiteralPath $Out -Value "KILL PID=$ProcessId limit reached"
      Stop-Process -InputObject $target -Force
    }
    break
  }
  Start-Sleep -Seconds 5
}
