# preparer_sd.ps1 -Sd D: [-Config benchmark|complet|normal|2coeurs]  -- copie BOOT.bin, model.bin, config.txt et cree images\ sur la carte SD
param([Parameter(Mandatory=$true)][string]$Sd, [string]$Config = "benchmark")
$ROOT = "C:\Users\USER\Desktop\yolo_hw"
$cfg = switch ($Config) { "benchmark" { "config_benchmark.txt" } "complet" { "config_complet_avec_scalaire.txt" } "2coeurs" { "config_2coeurs.txt" } default { "config_normal.txt" } }
Copy-Item "$ROOT\boot\BOOT.bin" "$Sd\BOOT.bin" -Force
Copy-Item "$ROOT\sw_net\model.bin" "$Sd\model.bin" -Force
Copy-Item "$ROOT\sw_sd\config_sd\$cfg" "$Sd\config.txt" -Force
New-Item -ItemType Directory -Force "$Sd\images" | Out-Null
Write-Host "Carte $Sd prete (config : $cfg). Images presentes :"
Get-ChildItem "$Sd\images" | Select-Object Name, Length
