# Launch a UE 5.8 editor for a runtime test without disturbing the user's desktop.
# - Moves the editor's windows to the side monitor (default: top-left 1080p, X=-1920 Y=-1080) without activating them.
# - Never minimises it: a minimised editor runs at ~3 fps and the plugin skips hidden Content Browsers (wrong data).
# - Backs up and restores the per-user files a run can change: FilterBar.json, EditorLayout.ini, EditorSettings.ini
#   (engine-wide BongThumbLive settings) and the project's EditorPerProjectUserSettings.ini.
# Note: PowerShell variables are case-insensitive; never name a window handle $h/$w/$x/$y here.
# Call it in-process (& .\btl_launch_bg.ps1 ...): with pwsh -File an -EditorArgs value starting with '-' is mis-parsed.
# Usage: & btl_launch_bg.ps1 -Project <uproject> -EditorArgs '<extra args>' [-LogCopy <path>] [-TimeoutSec 1500]
param(
	[Parameter(Mandatory)] [string]$Project,
	[string]$EditorArgs = '',
	[string]$LogCopy = '',
	[int]$TimeoutSec = 1500,
	[int]$X = -1920, [int]$Y = -1080, [int]$W = 1920, [int]$H = 1032
)
$ErrorActionPreference = 'Stop'
$Editor = 'K:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$ProjDir = Split-Path $Project
$ProjName = [IO.Path]::GetFileNameWithoutExtension($Project)

Add-Type -Namespace BTL -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
public struct RECT { public int L, T, R, B; }
'@

$backup = Join-Path $env:TEMP ("btl_bg_backup_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force $backup | Out-Null
$files = @(
	(Join-Path $env:LOCALAPPDATA 'UnrealEngine\Editor\FilterBar.json'),
	(Join-Path $env:LOCALAPPDATA 'UnrealEngine\5.8\Saved\Config\WindowsEditor\EditorLayout.ini'),
	# Engine-wide plugin settings live here (config=EditorSettings); test runs change them via BongThumbLive.Set.
	(Join-Path $env:LOCALAPPDATA 'UnrealEngine\5.8\Saved\Config\WindowsEditor\EditorSettings.ini'),
	(Join-Path $ProjDir 'Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini')
)
$saved = @{}
$i = 0
foreach ($f in $files) { if (Test-Path $f) { $dst = Join-Path $backup ("${i}_" + (Split-Path $f -Leaf)); Copy-Item $f $dst; $saved[$f] = $dst }; $i++ }

# Create the main window on the side monitor from the start. The editor main frame reads its first placement from
# [RootWindow] in the project's EditorPerProjectUserSettings.ini (FRootWindowLocation, DPI-unscaled; the side monitor
# is 100 %). Point it at the side monitor for this run; the file is restored afterwards. The move loop stays as fallback.
$ppIni = $files[3]
New-Item -ItemType Directory -Force (Split-Path $ppIni) | Out-Null
$lines = if (Test-Path $ppIni) { [IO.File]::ReadAllLines($ppIni) } else { @() }
$out = New-Object System.Collections.Generic.List[string]
$skip = $false
foreach ($l in $lines) {
	if ($l -match '^\[(.+)\]$') { $skip = ($matches[1] -eq 'RootWindow') }
	if (-not $skip) { $out.Add($l) }
}
$out.Add('[RootWindow]')
$out.Add("ScreenPosition=X=$X.000 Y=$Y.000")
$out.Add("WindowSize=X=$W.000 Y=$H.000")
$out.Add('InitiallyMaximized=False')
[IO.File]::WriteAllLines($ppIni, $out)
Write-Host "RootWindow: main window placed at $X,$Y ${W}x$H for this run"

$p = Start-Process $Editor -ArgumentList ("`"$Project`" -nosplash " + $EditorArgs) -PassThru
$moved = @{}
$lastRect = '(none)'
$peakPrivate = 0
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$SWP_NOACTIVATE = 0x0010; $SWP_NOZORDER = 0x0004
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
	$targets = New-Object System.Collections.Generic.List[IntPtr]
	[BTL.Win]::EnumWindows({ param($hw, $lp)
		$wpid = 0; [void][BTL.Win]::GetWindowThreadProcessId($hw, [ref]$wpid)
		if ($wpid -eq $p.Id -and [BTL.Win]::IsWindowVisible($hw)) { $targets.Add($hw) }
		return $true }, [IntPtr]::Zero) | Out-Null
	foreach ($hwnd in $targets) {
		$r = New-Object BTL.Win+RECT; [void][BTL.Win]::GetWindowRect($hwnd, [ref]$r)
		if ([BTL.Win]::IsIconic($hwnd) -or [BTL.Win]::IsZoomed($hwnd)) { [void][BTL.Win]::ShowWindow($hwnd, 4) } # SW_SHOWNOACTIVATE: never stay minimised; un-maximise so it can be moved
		$big = ($r.R - $r.L) -ge 800
		if ($big) { $lastRect = "$($r.L),$($r.T) $($r.R - $r.L)x$($r.B - $r.T)" }
		if ($big -and ($r.L -ne $X -or $r.T -ne $Y)) {
			[void][BTL.Win]::SetWindowPos($hwnd, [IntPtr]::Zero, $X, $Y, $W, $H, $SWP_NOACTIVATE -bor $SWP_NOZORDER)
			if (-not $moved.ContainsKey([int64]$hwnd)) {
				$moved[[int64]$hwnd] = 1
				$after = New-Object BTL.Win+RECT; [void][BTL.Win]::GetWindowRect($hwnd, [ref]$after)
				Write-Host "moved window $hwnd to $($after.L),$($after.T) $($after.R - $after.L)x$($after.B - $after.T)"
			}
		}
	}
	try { $p.Refresh(); $peakPrivate = [Math]::Max($peakPrivate, $p.PrivateMemorySize64) } catch {}
	Start-Sleep -Milliseconds 300
}
Write-Host "last main-window rect before exit: $lastRect"
Write-Host ("peakPrivateGB={0:N2}" -f ($peakPrivate / 1GB))
if (-not $p.HasExited) { Write-Host "TIMEOUT: editor pid $($p.Id) still running" } else { Write-Host "exit $($p.ExitCode)" }

foreach ($f in $saved.Keys) { Copy-Item $saved[$f] $f -Force }
Write-Host "restored $($saved.Count) per-user file(s) from $backup"
# Backup rotation: keep the three newest launcher backups.
Get-ChildItem $env:TEMP -Directory -Filter 'btl_bg_backup_*' | Sort-Object Name -Descending | Select-Object -Skip 3 | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
if ($LogCopy) { Copy-Item (Join-Path $ProjDir "Saved\Logs\$ProjName.log") $LogCopy -Force }
