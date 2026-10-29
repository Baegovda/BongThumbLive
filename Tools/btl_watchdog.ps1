param([int]$LimitGB=45,[int]$MinFreeGB=12,[string]$Out)
"time,ws_gb,private_gb,free_gb,gpu_ded_gb,gpu_shared_gb" | Set-Content $Out
for($i=0;$i -lt 2000;$i++){
  Start-Sleep 5
  $p=Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue | Select -First 1
  if(-not $p){ if($i -gt 24){ break } else { continue } }
  $free=(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1MB
  $ded=0;$sh=0
  try { $c=Get-Counter "\GPU Process Memory(pid_$($p.Id)*)\Dedicated Usage","\GPU Process Memory(pid_$($p.Id)*)\Shared Usage" -ErrorAction Stop; foreach($s in $c.CounterSamples){ if($s.Path -like '*dedicated*'){$ded+=$s.CookedValue}else{$sh+=$s.CookedValue} } } catch {}
  $line="{0},{1:N2},{2:N2},{3:N2},{4:N2},{5:N2}" -f (Get-Date -Format HH:mm:ss),($p.WorkingSet64/1GB),($p.PrivateMemorySize64/1GB),$free,($ded/1GB),($sh/1GB)
  Add-Content $Out $line
  if(($p.PrivateMemorySize64/1GB) -gt $LimitGB -or $free -lt $MinFreeGB){ Add-Content $Out "KILL limit reached"; Stop-Process -Id $p.Id -Force; Get-Process CrashReportClientEditor -ErrorAction SilentlyContinue | Stop-Process -Force; break }
}
