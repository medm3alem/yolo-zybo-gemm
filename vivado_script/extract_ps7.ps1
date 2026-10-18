# Extrait la config du PS7 depuis l'XSA d'Armin -> vivado_script\ps7_config.tcl
$ROOT   = "C:\Users\USER\Desktop\yolo_hw"
$src = "C:\Users\USER\Desktop\armin"
$xsa = Get-ChildItem $src -Recurse -Filter "zybo_100mhz.xsa" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $xsa) { $xsa = Get-ChildItem $src -Recurse -Filter "*.xsa" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $xsa) { throw "Aucun .xsa trouve sous $src (cherchez-le: Get-ChildItem C:\Users\USER -Recurse -Filter *.xsa)" }
Write-Host "XSA utilise : $xsa"
$tmp = "$ROOT\xsa_tmp"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $tmp | Out-Null
Copy-Item $xsa "$tmp\a.zip"
Expand-Archive "$tmp\a.zip" "$tmp\x" -Force
$hwh = Get-ChildItem "$tmp\x" -Recurse -Filter "*.hwh" | Select-Object -First 1
if (-not $hwh) { throw "pas de .hwh dans l'XSA" }
$xml = [xml](Get-Content $hwh.FullName -Raw)
$m = $xml.SelectSingleNode("//MODULE[@MODTYPE='processing_system7']")
if (-not $m) { throw "module processing_system7 introuvable dans $($hwh.Name)" }
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# genere depuis $xsa")
$n = 0
foreach ($node in $m.SelectNodes("PARAMETERS/PARAMETER")) {
  $name = $node.GetAttribute("NAME"); $val = $node.GetAttribute("VALUE")
  if ($name -like "PCW_*" -and $val -ne "" -and $val -notmatch '[{}\\]') {
    $lines.Add("catch {set_property CONFIG.$name {$val} `$ps}")
    $n++
  }
}
$lines | Set-Content "$ROOT\vivado_script\ps7_config.tcl" -Encoding ASCII
Write-Host "$n parametres PCW_ ecrits dans $ROOT\vivado_script\ps7_config.tcl"
Select-String -Path "$ROOT\vivado_script\ps7_config.tcl" -Pattern "PCW_UART1_PERIPHERAL_ENABLE|PCW_UIPARAM_DDR_PARTNO|PCW_FPGA0_PERIPHERAL_FREQMHZ"
