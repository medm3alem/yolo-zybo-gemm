# pipeline_complet.ps1 -- mesures sur la Zybo + lecture des resultats + graphes + archive, en une seule commande.
#
#   powershell -ExecutionPolicy Bypass -File .\pipeline_complet.ps1 -Port COM5
#   powershell -ExecutionPolicy Bypass -File .\pipeline_complet.ps1 -Port COM5 -Puissance      (ajoute les mesures de puissance au wattmetre)
#   powershell -ExecutionPolicy Bypass -File .\pipeline_complet.ps1 -Etapes figures            (refait seulement les graphes)
#
# Etapes (dans l'ordre) : couches, reseau, sd, puissance, vivado, figures, archive
#   couches  : test des 21 couches GEMM sur la carte (JTAG)       -> logs\serie_<date>_couches.txt
#   reseau   : reseau complet par couche, hash a verifier (JTAG)  -> logs\serie_<date>_reseau.txt
#   sd       : benchmark ARM seul / ARM+IP sur les images de la carte SD -> logs\resultats_<date>\
#   puissance: (option -Puissance) relevé au wattmetre USB           -> logs\mesures_puissance.csv
#   vivado   : copie vivado_util.rpt dans logs si on le trouve
#   figures  : make_figures.py -> rapport_figures\
#   archive  : rapport_figures + logs de ce passage -> envoi_rapport_<date>.zip (a m'envoyer)
param(
  [string]$Port = "",
  [string]$Root = "C:\Users\USER\Desktop\yolo_hw",
  [string[]]$Etapes = @("couches","reseau","sd","vivado","figures","archive"),
  [switch]$Puissance,
  [string]$Sd = "",
  [string]$Xsct = "",
  [int]$Baud = 115200,
  [int]$TimeoutSd = 1500,
  [string]$Cpu1Elf = "C:\Users\USER\gemm_dualcore\gemm_cpu1\Debug\gemm_cpu1.elf",
  [switch]$SansDeuxCoeurs
)
$ErrorActionPreference = "Stop"
# avec "powershell -File", -Etapes sd,figures arrive en UN seul texte : on le redecoupe
$Etapes = @($Etapes | ForEach-Object { $_ -split "[,;\s]+" } | Where-Object { $_ })
$TS   = Get-Date -Format "yyyyMMdd_HHmmss"
$LOGS = "$Root\logs"; New-Item -ItemType Directory -Force $LOGS | Out-Null
$FIG  = "$Root\rapport_figures"
$RUN  = "$LOGS\pipeline_$TS"; New-Item -ItemType Directory -Force $RUN | Out-Null
$SUM  = New-Object System.Collections.ArrayList
function Say($m, $c = "Cyan") { Write-Host $m -ForegroundColor $c; [void]$SUM.Add($m) }
function Warn($m) { Say "ATTENTION : $m" "Yellow" }
function Fail($m) { Say "ERREUR : $m" "Red"; $SUM | Set-Content "$RUN\resume_pipeline.txt" -Encoding UTF8; exit 1 }
function Has($e) { $Etapes -contains $e }
if ($Puissance -and -not (Has "puissance")) { $Etapes = $Etapes + "puissance" }
# l'ordre est toujours celui de la liste complete
$ORDRE = "couches","reseau","sd","puissance","vivado","figures","archive"

function Find-File($name, $dirs) {
  foreach ($d in $dirs) { if (Test-Path $d) { $f = Get-ChildItem $d -Recurse -Filter $name -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch "vitis_ws" } | Select-Object -First 1; if ($f) { return $f.FullName } } }
  return $null
}

# ---------------------------------------------------------------- outils : xsct, port serie
if (-not $Xsct) {
  # le vrai lanceur est bin\xsct.bat (pas bin\scripts\xsct.bat, qui ne marche pas seul)
  foreach ($c in @("C:\Xilinx\Vitis\2023.1\bin\xsct.bat")) { if (Test-Path $c) { $Xsct = $c; break } }
  if (-not $Xsct) {
    $f = Get-ChildItem "C:\Xilinx" -Recurse -Filter xsct.bat -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch "\\scripts\\" } | Select-Object -First 1
    if ($f) { $Xsct = $f.FullName }
  }
}
$besoinCarte = (Has "couches") -or (Has "reseau") -or (Has "sd") -or (Has "puissance")
if ($besoinCarte) {
  if (-not $Xsct) { Fail "xsct introuvable (ajouter C:\Xilinx\Vitis\2023.1\bin au PATH ou passer -Xsct <chemin>)" }
  if (-not $Port) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 1) { $Port = $ports[0] } else { Write-Host "Ports serie : $($ports -join ', ')"; $Port = Read-Host "Port serie de la Zybo (ex. COM5)" }
  }
  Say "xsct : $Xsct ; port serie : $Port @ $Baud"
  # ---- verifications avant de commencer
  if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) { Fail "le port $Port n'existe pas (ports : $([System.IO.Ports.SerialPort]::GetPortNames() -join ', '))" }
  $ctcl = "$env:TEMP\pipeline_check.tcl"; @("connect","puts [targets]","exit") | Set-Content $ctcl -Encoding ASCII
  $ErrorActionPreference = "Continue"; $o = (& $Xsct $ctcl 2>&1 | Out-String); $ErrorActionPreference = "Stop"
  if ($o -notmatch "xc7z" -or $o -notmatch "ARM") { Write-Host $o -ForegroundColor DarkGray; Fail "xsct ne voit pas la Zybo en JTAG (alimentation ? cable USB/JTAG ? cavalier JP5 sur JTAG ?)" }
  Say "Verification : Zybo vue en JTAG, port $Port present."
  foreach ($n in @("$Root\sw_net\model.bin")) { if (-not (Test-Path $n)) { Fail "fichier absent : $n" } }
}

# Ouvre le port, lance xsct (qui reset la carte), capture jusqu'au marqueur, ecrit le log.
function Run-Capture($tcl, $logfile, $marker, $timeout, $xargs = @()) {
  $sp = New-Object System.IO.Ports.SerialPort $Port,$Baud,"None",8,"One"
  $sp.ReadTimeout = 200; $sp.Open(); $sp.DiscardInBuffer()
  $sb = New-Object System.Text.StringBuilder
  try {
    Say "  xsct $(Split-Path $tcl -Leaf) ..."
    $ErrorActionPreference = "Continue"
    $xo = (& $Xsct $tcl @xargs 2>&1 | ForEach-Object { Write-Host "  [xsct] $_" -ForegroundColor DarkGray; $_ } | Out-String)
    $ErrorActionPreference = "Stop"
    if ($xo -match "n'est pas reconnu|is not recognized|ERREUR|ERROR|no targets found|Cannot|couldn't") { $sp.Close(); Fail "xsct a echoue (voir lignes [xsct] ci-dessus) : arret pour ne pas attendre le timeout" }
    $t0 = Get-Date; $fin = $false
    while (((Get-Date) - $t0).TotalSeconds -lt $timeout) {
      $s = $sp.ReadExisting()
      if ($s) { [void]$sb.Append($s); Write-Host -NoNewline $s; if ($sb.ToString() -match $marker) { $fin = $true; break } }
      else { Start-Sleep -Milliseconds 100 }
    }
    if (-not $fin) { Warn "marqueur '$marker' non vu apres $timeout s (log enregistre quand meme)" }
  } finally { $sp.Close() }
  $sb.ToString() | Set-Content $logfile -Encoding UTF8
  Write-Host ""
  return $sb.ToString()
}

# ---------------------------------------------------------------- etapes JTAG
function Has-Elf($app) { return [bool](Get-ChildItem "$Root\vitis_ws\$app" -Recurse -Filter "$app.elf" -ErrorAction SilentlyContinue | Select-Object -First 1) }
$logCouches = $null; $logReseau = $null; $resDir = $null; $vivadoRpt = $null
if ((Has "couches") -and -not (Has-Elf "test_tile")) { Warn "test_tile.elf absent de vitis_ws (efface lors de la compilation suivante) : etape couches ignoree, le dernier log serie_*.txt existant sera utilise pour les graphes" }
elseif (Has "couches") {
  Say "`n=== [1] Test des 21 couches GEMM (ARM + IP) ===" "Green"
  $tcl = Find-File "run_test.tcl" @("$Root\sw","$Root")
  if (-not $tcl) { Fail "run_test.tcl introuvable sous $Root" }
  $logCouches = "$LOGS\serie_${TS}_couches.txt"
  $t = Run-Capture $tcl $logCouches "=== Fin ===" 120
  if ($t -match "BILAN : TOUT OK") { Say "Couches : TOUT OK ($logCouches)" } else { Warn "BILAN : TOUT OK absent -> verifier $logCouches" }
}
if ((Has "reseau") -and -not (Has-Elf "yolo_run")) { Warn "yolo_run.elf absent de vitis_ws : etape reseau ignoree, le dernier log existant sera utilise pour les graphes" }
elseif (Has "reseau") {
  Say "`n=== [2] Reseau complet par couche (ARM + IP, JTAG) ===" "Green"
  $tcl = Find-File "run_yolo.tcl" @("$Root\sw_net","$Root")
  if (-not $tcl) { Fail "run_yolo.tcl introuvable sous $Root" }
  $logReseau = "$LOGS\serie_${TS}_reseau.txt"
  $t = Run-Capture $tcl $logReseau "=== Fin ===" 180
  if ($t -match "IDENTIQUE") { Say "Reseau : hash IDENTIQUE a la reference numpy ($logReseau)" } else { Warn "hash non IDENTIQUE -> resultats a ne pas utiliser ($logReseau)" }
  $m = [regex]::Match($t, "TOTAL,[^\r\n]*"); if ($m.Success) { Say $m.Value }
}

# ---------------------------------------------------------------- etape SD (aussi utilisee pour la puissance)
function Find-Card {
  if ($Sd) { return $Sd.TrimEnd('\') }
  $v = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq "BOOTFS" -and $_.DriveLetter } | Select-Object -First 1
  if ($v) { return "$($v.DriveLetter):" }
  return $null
}
function Wait-Card($msg) {
  while ($true) {
    Read-Host $msg | Out-Null
    $c = Find-Card
    if ($c -and (Test-Path "$c\")) { return $c }
    Write-Host "Carte (volume BOOTFS) non detectee, reessayer (ou relancer avec -Sd D:)" -ForegroundColor Yellow
  }
}
# Un passage complet : carte dans le PC -> preparee -> carte dans la Zybo -> run JTAG -> carte dans le PC -> out\ copie
function Run-Sd($configFile, $nomDest, $conseil, $timeout, $tclName = "run_yolo_sd.tcl", $xargs = @()) {
  $cfgSrc = Find-File $configFile @("$Root\sw_sd\config_sd","$PSScriptRoot\config_puissance","$Root\pipeline\config_puissance","$PSScriptRoot")
  if (-not $cfgSrc) { Fail "$configFile introuvable" }
  $card = Wait-Card "Carte SD dans le PC, puis Entree"
  Say "Carte : $card ; config : $configFile"
  $boot = "$Root\boot\BOOT.bin"; if (Test-Path $boot) { Copy-Item $boot "$card\BOOT.bin" -Force } else { Warn "BOOT.bin absent (inutile en mode JTAG)" }
  Copy-Item "$Root\sw_net\model.bin" "$card\model.bin" -Force
  Copy-Item $cfgSrc "$card\config.txt" -Force
  New-Item -ItemType Directory -Force "$card\images" | Out-Null
  $imgs = @(Get-ChildItem "$card\images" -File | Where-Object { $_.Extension -match "jpg|jpeg|png|bmp" })
  if ($imgs.Count -eq 0) { Fail "aucune image dans $card\images" }
  Say "Images : $($imgs.Count) ($(($imgs | ForEach-Object Name) -join ', '))"
  if (Test-Path "$card\out") { Rename-Item "$card\out" "out_ancien_$TS" }       # les anciens resultats sont gardes, pas effaces
  $held = @()
  if ($nomDest -like "puissance*" -and $imgs.Count -gt 1) {                    # pour la puissance : une seule image, sinon la mesure dure trop longtemps
    New-Item -ItemType Directory -Force "$card\images_hold" | Out-Null
    foreach ($f in $imgs | Select-Object -Skip 1) { Move-Item $f.FullName "$card\images_hold\" -Force; $held += $f.Name }
    Say "Mesure de puissance : seule $($imgs[0].Name) est utilisee, les autres sont dans images_hold\ (remises ensuite)."
  }
  Say "Ejecter la carte proprement, la mettre dans la Zybo (JP5 sur JTAG ou SD, peu importe), carte allumee et reliee en JTAG + USB serie."
  Read-Host "Entree quand la carte est en place" | Out-Null
  $tcl = Find-File $tclName @("$Root\sw_sd","$Root")
  if (-not $tcl) { Fail "$tclName introuvable (copier le contenu de yolo_hw_2coeurs\sw_sd dans yolo_hw\sw_sd)" }
  if ($conseil) { Say $conseil "Magenta" }
  $log = "$RUN\serie_$nomDest.txt"
  $t = Run-Capture $tcl $log "Fin : resultats" $timeout $xargs
  $ok = $t -match "Fin : resultats"
  if (-not $ok) { Warn "fin de traitement non vue (voir $log)" }
  $card = Wait-Card "Remettre la carte dans le PC (apres LED verte fixe), puis Entree"
  if ($held.Count) { foreach ($n in $held) { Move-Item "$card\images_hold\$n" "$card\images\" -Force }; Remove-Item "$card\images_hold" -Force -ErrorAction SilentlyContinue }
  if (-not (Test-Path "$card\out")) { Fail "pas de dossier out\ sur la carte : le traitement n'a pas abouti" }
  $dest = "$LOGS\$nomDest"; New-Item -ItemType Directory -Force $dest | Out-Null
  Copy-Item "$card\out\*" $dest -Recurse -Force
  Copy-Item $log $dest -Force
  Say "Resultats copies : $dest"
  return $dest
}

if ((Has "sd") -and -not (Has-Elf "yolo_sd")) { Fail "yolo_sd.elf absent de vitis_ws : compiler d'abord (build_yolo_sd.tcl)" }
if (Has "sd") {
  Say "`n=== [3] Benchmark sur images de la carte SD : ARM seul (NEON) vs ARM + IP ===" "Green"
  $deux = (-not $SansDeuxCoeurs) -and (Test-Path $Cpu1Elf)
  if ($deux) { Say "Mode 2 coeurs inclus (programme du coeur 1 : $Cpu1Elf, a reconstruire avec gemm_cpu1_full.c de yolo_hw_2coeurs\cpu1)" }
  else {
    $raison = "programme du coeur 1 introuvable : $Cpu1Elf"
    if ($SansDeuxCoeurs) { $raison = "option -SansDeuxCoeurs" }
    Warn "mode 2 coeurs non execute ($raison)"
  }
  $resDir = if ($deux) { Run-Sd "config_2coeurs.txt" "resultats_$TS" "ARM seul 1 coeur, ARM seul 2 coeurs et ARM + IP, plusieurs fois par image : patienter jusqu'a la LED verte fixe (environ 4 a 5 minutes)." $TimeoutSd "run_yolo_sd_2c.tcl" @($Cpu1Elf) }
            else { Run-Sd "config_benchmark.txt" "resultats_$TS" "Chaque image est traitee plusieurs fois par mode : patienter jusqu'a la LED verte fixe (suivre le journal ci-dessous)." $TimeoutSd }
  if ($deux) {
    $lj = Get-Content "$resDir\serie_resultats_$TS.txt" -Raw -ErrorAction SilentlyContinue
    if ($lj -match "Deux coeurs : (\d+) produits envoyes au coeur 1, (\d+) repasses") { Say "Deux coeurs : $($Matches[1]) produits sur le coeur 1, $($Matches[2]) repasses sur un coeur" ; if ([int]$Matches[2] -gt 0) { Warn "le coeur 1 a cesse de repondre : mesure 2 coeurs non valide" } }
    elseif ($lj -match "neon2 indisponible|ne repond pas|aucune strategie") { Warn "mode 2 coeurs indisponible (voir le journal) : gemm_cpu1 reconstruit avec le noyau 4 ? coeur 1 charge ?" }
  }
  $b = "$resDir\benchmark.csv"
  if (Test-Path $b) {
    $rows = Import-Csv $b; $rows | Format-Table image, mode, repetitions, reseau_moy_ms, reseau_ecart_ms, produits_matriciels_ms, sorties_identiques_au_premier_mode -AutoSize | Out-String | Write-Host
    if ($deux -and -not ($rows | Where-Object { $_.mode -eq "neon2" })) { Warn "aucune ligne neon2 dans benchmark.csv" }
    $bad = @($rows | Where-Object { $_.sorties_identiques_au_premier_mode -notmatch "oui|yes|1|true" -and $_.mode -ne $rows[0].mode })
    if ($bad.Count) { Warn "$($bad.Count) ligne(s) avec sorties differentes entre modes" } else { Say "Sorties identiques entre ARM seul et ARM + IP sur toutes les images." }
  } else { Warn "benchmark.csv absent" }
}

# ---------------------------------------------------------------- puissance
if (Has "puissance") {
  Say "`n=== [4] Puissance (wattmetre USB entre l'alimentation et la carte) ===" "Green"
  $P = @{}
  $v = Read-Host "Brancher le wattmetre. Carte allumee, programme TERMINE (LED verte fixe) ou a vide : puissance au repos en W"
  $P["repos"] = $v
  $d = Run-Sd "config_puissance_arm.txt" "puissance_arm_$TS" "Calcul ARM seul pendant ~4 min : relever la puissance STABLE affichee (ignorer les 10 premieres secondes)." 600
  $P["arm"] = Read-Host "Puissance relevee pendant le calcul ARM seul (W)"
  $d = Run-Sd "config_puissance_ip.txt" "puissance_ip_$TS" "Calcul ARM + IP pendant ~1,5 min : relever la puissance STABLE." 400
  $P["ip"] = Read-Host "Puissance relevee pendant le calcul ARM + IP (W)"
  $lines = @("mode,puissance_w"); foreach ($k in "repos","arm","ip") { $lines += "$k,$(($P[$k] -replace ',','.'))" }
  $lines | Set-Content "$LOGS\mesures_puissance.csv" -Encoding UTF8
  Say "Puissances : $($lines -join ' | ')"
}

# ---------------------------------------------------------------- Vivado
if (Has "vivado") {
  Say "`n=== [5] Rapport d'utilisation Vivado ===" "Green"
  $dst = "$LOGS\vivado_util.rpt"
  if (Test-Path $dst) { Say "deja present : $dst" } else {
    $f = Get-ChildItem "$Root\vivado" -Recurse -Include "*utilization*.rpt","vivado_util.rpt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) { Copy-Item $f.FullName $dst; Say "copie : $($f.FullName)" } else { Warn "aucun rapport d'utilisation trouve (dans Vivado : report_utilization -file vivado_util.rpt)" }
  }
}

# ---------------------------------------------------------------- figures
if (Has "figures") {
  Say "`n=== [6] Graphes ===" "Green"
  python -c "import matplotlib, numpy" 2>$null
  if ($LASTEXITCODE -ne 0) { Say "Installation de matplotlib et numpy..."; python -m pip install matplotlib numpy }
  $mk = Find-File "make_figures.py" @($PSScriptRoot,"$Root\sw_sd","$Root")
  if (-not $mk) { Fail "make_figures.py introuvable (le copier a cote de ce script)" }
  $args2 = @($mk, "--root", $Root, "--out", $FIG)
  if ($resDir) { $args2 += @("--results", $resDir) }
  if ($logCouches) { $args2 += @("--couches", $logCouches) }
  if ($logReseau)  { $args2 += @("--e2e", $logReseau) }
  if (Test-Path "$LOGS\vivado_util.rpt") { $args2 += @("--vivado", "$LOGS\vivado_util.rpt") }
  if (Test-Path "$LOGS\mesures_puissance.csv") { $args2 += @("--puissance", "$LOGS\mesures_puissance.csv") }
  python @args2
  if ($LASTEXITCODE -ne 0) { Fail "make_figures.py a echoue" }
}

# ---------------------------------------------------------------- archive
if (Has "archive") {
  Say "`n=== [7] Archive ===" "Green"
  $info = @("Pipeline $TS", "Utilisateur : $env:USERNAME", "Machine : $env:COMPUTERNAME", "Etapes : $($Etapes -join ',')",
            "model.bin md5 : $((Get-FileHash "$Root\sw_net\model.bin" -Algorithm MD5 -ErrorAction SilentlyContinue).Hash)",
            "BOOT.bin md5 : $((Get-FileHash "$Root\boot\BOOT.bin" -Algorithm MD5 -ErrorAction SilentlyContinue).Hash)")
  $info | Set-Content "$RUN\info_run.txt" -Encoding UTF8
  $SUM | Set-Content "$RUN\resume_pipeline.txt" -Encoding UTF8
  foreach ($f in @($logCouches, $logReseau, "$LOGS\vivado_util.rpt", "$LOGS\mesures_puissance.csv")) { if ($f -and (Test-Path $f)) { Copy-Item $f $RUN -Force } }
  if ($resDir -and (Test-Path $resDir)) { Copy-Item $resDir "$RUN\$(Split-Path $resDir -Leaf)" -Recurse -Force }
  $stage = "$RUN\rapport_figures"; New-Item -ItemType Directory -Force $stage | Out-Null
  if (Test-Path $FIG) { Copy-Item "$FIG\*" $stage -Recurse -Force }
  $zip = "$Root\envoi_rapport_$TS.zip"
  Compress-Archive -Path "$RUN\*" -DestinationPath $zip -Force
  Say "Archive a m'envoyer : $zip"
}
$SUM | Set-Content "$RUN\resume_pipeline.txt" -Encoding UTF8
Say "`nTermine. Figures : $FIG ; dossier du passage : $RUN" "Green"
