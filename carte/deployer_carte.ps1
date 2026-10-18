# deployer_carte.ps1 -- prepare la carte SD pour un demarrage AUTONOME de la Zybo (sans PC).
# Compile le programme si besoin, fabrique BOOT.bin, vide out\, copie vos images, model.bin et config.txt.
#
# Exemples (PowerShell) :
#   powershell -ExecutionPolicy Bypass -File .\deployer_carte.ps1 -Images "$HOME\Pictures\test"
#   powershell -ExecutionPolicy Bypass -File .\deployer_carte.ps1                       (garde les images deja sur la carte)
#   powershell -ExecutionPolicy Bypass -File .\deployer_carte.ps1 -Config benchmark -Reconstruire
# Parametres :
#   -Sd D:               lettre de la carte (par defaut : volume BOOTFS, sinon l'unique disque amovible)
#   -Images <dossier>    dossier de vos images (jpg, jpeg, png, bmp) a copier dans images\ de la carte (remplace les anciennes)
#   -Config normal|benchmark|complet   normal = IP, 1 passage ; benchmark = IP + ARM NEON, 5 passages ; complet = avec ARM scalaire
#   -Reconstruire        recompile le programme meme s'il existe deja
#   -SansEjecter         ne pas ejecter la carte a la fin
param(
  [string]$Sd = "", [string]$Images = "", [string]$Config = "normal",
  [switch]$Reconstruire, [switch]$SansEjecter,
  [string]$Root = "C:\Users\USER\Desktop\yolo_hw",
  [string]$Xsct = "C:\Xilinx\Vitis\2023.1\bin\xsct.bat"
)
$ErrorActionPreference = "Stop"
function Etape($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
function Fail($m)  { Write-Host "ERREUR : $m" -ForegroundColor Red; exit 1 }

# ---- 0. verifications
Etape "[0] Verifications"
if ($Config -eq "2coeurs") { Write-Host "ATTENTION : le mode 2 coeurs ne marche pas en demarrage autonome (le coeur 1 a besoin du PC/JTAG). Utilisez pipeline\pipeline_complet.ps1. -> config 'benchmark' utilisee." -ForegroundColor Yellow; $Config = "benchmark" }
$cfgFile = switch ($Config) { "normal" { "config_normal.txt" } "benchmark" { "config_benchmark.txt" } "complet" { "config_complet_avec_scalaire.txt" } default { Fail "config inconnue : $Config (normal, benchmark ou complet)" } }
foreach ($f in @("$Root\sw_net\model.bin","$Root\sw_sd\config_sd\$cfgFile","$Root\sw_sd\make_boot.ps1","$Root\vivado\design_1_wrapper.xsa")) { if (-not (Test-Path $f)) { Fail "fichier absent : $f" } }
if ($Images -and -not (Test-Path $Images)) { Fail "dossier d'images introuvable : $Images" }

# ---- 1. carte SD
if (-not $Sd) {
  $v = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -eq "BOOTFS" })
  if ($v.Count -eq 0) { $v = @(Get-Volume | Where-Object { $_.DriveLetter -and $_.DriveType -eq "Removable" }) }
  if ($v.Count -ne 1) { Fail "carte SD non identifiee ($($v.Count) candidat(s)). Relancez avec -Sd D: (lettre de la carte)." }
  $Sd = "$($v[0].DriveLetter):"
}
$Sd = $Sd.TrimEnd("\")
if (-not (Test-Path "$Sd\")) { Fail "carte introuvable : $Sd" }
$vol = Get-Volume -DriveLetter $Sd.Substring(0,1) -ErrorAction SilentlyContinue
if ($vol -and $vol.FileSystem -notmatch "FAT") { Fail "la carte $Sd est en $($vol.FileSystem) : il faut du FAT32 (la Zybo ne lit pas le NTFS/exFAT)." }
Write-Host "Carte SD : $Sd  ($($vol.FileSystemLabel), $($vol.FileSystem))"

# ---- 2. programme
Etape "[1] Programme (ELF)"
$elfPath = { Get-ChildItem "$Root\vitis_ws\yolo_sd" -Recurse -Filter yolo_sd.elf -ErrorAction SilentlyContinue | Select-Object -First 1 }
$elf = & $elfPath
$srcRecent = (Get-ChildItem "$Root\sw_sd" -Include *.c,*.h -Recurse | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
if ($elf -and -not $Reconstruire -and $elf.LastWriteTime -lt $srcRecent) { Write-Host "Sources plus recentes que l'ELF : reconstruction."; $Reconstruire = $true }
if (-not $elf -or $Reconstruire) {
  if (-not (Test-Path $Xsct)) { Fail "xsct introuvable : $Xsct" }
  if (Test-Path "$Root\vitis_ws") { Remove-Item "$Root\vitis_ws" -Recurse -Force }
  Write-Host "Compilation (quelques minutes)..."
  $log = "$Root\logs\build_$(Get-Date -Format yyyyMMdd_HHmmss).txt"
  New-Item -ItemType Directory -Force "$Root\logs" | Out-Null
  & $Xsct "$Root\sw_sd\build_yolo_sd.tcl" *> $log
  $elf = & $elfPath
  if (-not $elf) { Fail "compilation echouee, voir $log" }
}
Write-Host "ELF : $($elf.FullName)"

# ---- 3. BOOT.bin
Etape "[2] BOOT.bin (FSBL + bitstream + programme)"
$boot = "$Root\boot\BOOT.bin"
if ((-not (Test-Path $boot)) -or (Get-Item $boot).LastWriteTime -lt $elf.LastWriteTime -or $Reconstruire) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File "$Root\sw_sd\make_boot.ps1"
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $boot)) { Fail "BOOT.bin non cree" }
} else { Write-Host "BOOT.bin a jour : $boot" }

# ---- 4. contenu de la carte
Etape "[3] Copie sur la carte $Sd"
Copy-Item $boot "$Sd\BOOT.bin" -Force
Copy-Item "$Root\sw_net\model.bin" "$Sd\model.bin" -Force
Copy-Item "$Root\sw_sd\config_sd\$cfgFile" "$Sd\config.txt" -Force
if (Test-Path "$Sd\out") { Remove-Item "$Sd\out\*" -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force "$Sd\out" | Out-Null
New-Item -ItemType Directory -Force "$Sd\images" | Out-Null
if ($Images) {
  Remove-Item "$Sd\images\*" -Recurse -Force -ErrorAction SilentlyContinue
  $imgs = @(Get-ChildItem $Images -File | Where-Object { $_.Extension -match '^\.(jpg|jpeg|png|bmp)$' })
  if ($imgs.Count -eq 0) { Fail "aucune image (jpg, jpeg, png, bmp) dans $Images" }
  $imgs | Copy-Item -Destination "$Sd\images" -Force
}
$n = @(Get-ChildItem "$Sd\images" -File).Count
if ($n -eq 0) { Fail "aucune image dans $Sd\images : relancez avec -Images <dossier>" }
if ($n -gt 64) { Write-Host "ATTENTION : $n images, seules les 64 premieres sont traitees." -ForegroundColor Yellow }
Write-Host "Config : $cfgFile ; images ($n) :"; Get-ChildItem "$Sd\images" -File | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "Contenu de la carte :"; Get-ChildItem $Sd | Select-Object Name | Format-Table -AutoSize | Out-String | Write-Host

# ---- 5. ejection
if (-not $SansEjecter) {
  try { (New-Object -ComObject Shell.Application).Namespace(17).ParseName("$Sd\").InvokeVerb("Eject"); Write-Host "Carte ejectee." }
  catch { Write-Host "Ejection automatique impossible : ejectez la carte par l'icone de la barre des taches." -ForegroundColor Yellow }
}

Write-Host @"

=== Carte prete. Maintenant ===
 1. Eteindre la Zybo, inserer la carte SD (dessous de la carte).
 2. Cavalier JP5 sur SD.  Alimentation par USB (ou prise), interrupteur sur ON.
 3. LED verte LD4 : clignote lentement = calcul en cours ; FIXE = termine ; clignote vite = erreur.
 4. Eteindre, retirer la carte, la remettre dans le PC, puis lire les resultats :
      powershell -ExecutionPolicy Bypass -File "$Root\carte\lire_resultats_carte.ps1"
"@ -ForegroundColor Green
