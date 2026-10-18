# lire_resultats_carte.ps1 [-Sd D:] -- apres un passage sur la Zybo : affiche les resultats de out\ et les copie dans logs\
param([string]$Sd = "", [string]$Root = "C:\Users\USER\Desktop\yolo_hw")
if (-not $Sd) {
  $v = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -eq "BOOTFS" })
  if ($v.Count -eq 0) { $v = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.DriveType -eq "Removable" }) }
  if ($v.Count -ne 1) { Write-Host "Carte non identifiee : relancez avec -Sd D:"; exit 1 }
  $Sd = "$($v[0].DriveLetter):"
}
& powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\sw_sd\lire_resultats.ps1" -Sd $Sd
