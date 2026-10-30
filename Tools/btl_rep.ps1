function Rep([string]$p, [string]$old, [string]$new) {
  $b=[IO.File]::ReadAllBytes($p); $bom=($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
  $t=[IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)
  $old=$old.Replace("`r`n","`n"); $new=$new.Replace("`r`n","`n")
  $idx=$t.IndexOf($old); if($idx -lt 0){ throw "NOT FOUND in $p : $($old.Substring(0,[Math]::Min(80,$old.Length)))" }
  if($t.IndexOf($old,$idx+1) -ge 0){ throw "MULTIPLE in $p : $($old.Substring(0,[Math]::Min(80,$old.Length)))" }
  $t=$t.Substring(0,$idx)+$new+$t.Substring($idx+$old.Length)
  [IO.File]::WriteAllText($p,$t,(New-Object Text.UTF8Encoding($bom)))
  "ok: $p"
}
