# lire_resultats_carte.ps1 [-Sd D:] -- apres un passage sur la Zybo : copie out\ dans logs\ ; affiche le resume (mode benchmark) ou ouvre les images (mode production)
param([string]$Sd = "", [string]$Root = "C:\Users\USER\Desktop\yolo_hw")
if (-not $Sd) {
  $v = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -eq "BOOTFS" })
  if ($v.Count -eq 0) { $v = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.DriveType -eq "Removable" }) }
  if ($v.Count -ne 1) { Write-Host "Carte non identifiee : relancez avec -Sd D:"; exit 1 }
  $Sd = "$($v[0].DriveLetter):"
}
if (Test-Path "$Sd\out\benchmark.csv") { & powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\sw_sd\lire_resultats.ps1" -Sd $Sd; exit 0 }
$bmp = @(Get-ChildItem "$Sd\out" -Filter "*_det.bmp" -ErrorAction SilentlyContinue)
if ($bmp.Count -eq 0) { Write-Host "Rien dans $Sd\out : la Zybo a-t-elle fini (LED verte fixe) ?"; exit 1 }
$dest = "$Root\logs\production_" + (Get-Date -Format "yyyyMMdd_HHmmss")
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$Sd\out\*_det.bmp" $dest -Force
Write-Host "Mode production : $($bmp.Count) image(s) annotee(s) copiee(s) dans $dest"
Invoke-Item $dest
