# make_boot.ps1 -- fabrique BOOT.bin (FSBL + bitstream + programme yolo_sd) pour demarrer seul depuis la carte SD
# usage :  powershell -ExecutionPolicy Bypass -File .\make_boot.ps1            (cree yolo_hw\boot\BOOT.bin)
#          powershell -ExecutionPolicy Bypass -File .\make_boot.ps1 -Sd E:     (et le copie a la racine de la carte E:)
param([string]$Sd = "")
$ROOT = "C:\Users\USER\Desktop\yolo_hw"; $WS = "$ROOT\vitis_ws"; $OUT = "$ROOT\boot"
New-Item -ItemType Directory -Force $OUT | Out-Null

$fsblAll = @(Get-ChildItem -Path "$WS\plat_tile" -Recurse -Filter fsbl.elf -ErrorAction SilentlyContinue)
$fsbl = ($fsblAll | Where-Object { $_.FullName -match "export" } | Select-Object -First 1)
if (-not $fsbl) { $fsbl = $fsblAll | Select-Object -First 1 }
$bit  = Get-ChildItem -Path "$WS\plat_tile\hw" -Filter *.bit -ErrorAction SilentlyContinue | Select-Object -First 1
$elf  = Get-ChildItem -Path "$WS\yolo_sd" -Recurse -Filter yolo_sd.elf -ErrorAction SilentlyContinue | Select-Object -First 1
$bg   = Get-ChildItem -Path "C:\Xilinx\Vitis\2023.1\bin" -Recurse -Include bootgen.bat,bootgen.exe -ErrorAction SilentlyContinue | Sort-Object Extension | Select-Object -First 1
foreach ($p in @(@("fsbl.elf",$fsbl),@("bitstream",$bit),@("yolo_sd.elf",$elf),@("bootgen",$bg))) { if (-not $p[1]) { Write-Host "ERREUR : introuvable : $($p[0])"; exit 1 } }
Write-Host "FSBL      : $($fsbl.FullName)"; Write-Host "bitstream : $($bit.FullName)"; Write-Host "programme : $($elf.FullName)"; Write-Host "bootgen   : $($bg.FullName)"

$bif = "$OUT\boot.bif"
$txt = "the_ROM_image:`n{`n`t[bootloader]" + ($fsbl.FullName -replace '\\','/') + "`n`t" + ($bit.FullName -replace '\\','/') + "`n`t" + ($elf.FullName -replace '\\','/') + "`n}`n"
[System.IO.File]::WriteAllText($bif, $txt, (New-Object System.Text.ASCIIEncoding))
Remove-Item "$OUT\BOOT.bin" -ErrorAction SilentlyContinue
& $bg.FullName -arch zynq -image $bif -w on -o "$OUT\BOOT.bin"
if (-not (Test-Path "$OUT\BOOT.bin")) { Write-Host "ERREUR : BOOT.bin non cree"; exit 1 }
Write-Host ("OK : {0} ({1:N0} octets)" -f "$OUT\BOOT.bin", (Get-Item "$OUT\BOOT.bin").Length)
if ($Sd) { Copy-Item "$OUT\BOOT.bin" "$Sd\BOOT.bin" -Force; Write-Host "Copie dans $Sd\BOOT.bin" }
