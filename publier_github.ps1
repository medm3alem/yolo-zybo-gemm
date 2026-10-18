# publier_github.ps1 -- met yolo_hw dans un depot Git et le pousse sur GitHub.
#   Apercu (ne change rien)  :  powershell -ExecutionPolicy Bypass -File .\publier_github.ps1
#   Creer et pousser (prive) :  powershell -ExecutionPolicy Bypass -File .\publier_github.ps1 -Pousser
#   Depot public             :  ... -Pousser -Public        Autre nom : -Nom mon-depot
# Prerequis : Git (git --version). Pour que le depot GitHub soit cree automatiquement : GitHub CLI (gh) connecte (gh auth login).
# Sans gh : le script prepare tout et vous donne les 3 commandes pour pousser a la main.
param([string]$Root = "C:\Users\USER\Desktop\yolo_hw", [string]$Nom = "yolo-zybo-gemm", [string]$Compte = "", [switch]$Pousser, [switch]$Public)
$ErrorActionPreference = "Stop"
function Fail($m) { Write-Host "ERREUR : $m" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Root)) { Fail "dossier introuvable : $Root" }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "Git n'est pas installe (https://git-scm.com/download/win), puis relancez." }
Set-Location $Root

# ---- .gitignore : on garde les sources, scripts, BOOT.bin et model.bin ; on exclut les generes et les temporaires
$ign = @'
# genere par Vitis / Vivado (on ne garde que le .xsa de vivado\)
vitis_ws/
.Xil/
**/.Xil/
vivado/*
!vivado/design_1_wrapper.xsa
*.dcp
*.elf
*.jou
*.log
# poids Darknet officiels (24 Mo, a telecharger ; model.bin en est derive)
*.weights
# journaux et archives de mesures (le kit contient les resultats de reference)
logs/
_a_supprimer/
envoi_rapport_*.zip
*.zip
# Python
__pycache__/
*.pyc
# photos personnelles de test
WIN_*.jpg
resultats/
figures/
'@
Set-Content ".gitignore" $ign -Encoding UTF8

# ---- README.md
$readme = @'
# YOLOv4-tiny int8 sur Zybo Z7-20 : ARM Cortex-A9 vs IP FPGA `gemm_tile`

Projet CSC_5RO06_TA / ROB306 (ENSTA Paris) : accelerer le produit matriciel (GEMM) de YOLOv4-tiny int8 sur la Zybo Z7-20,
en comparant ARM seul (NEON, 1 et 2 coeurs) et ARM + IP FPGA. Partie ARM9 et application reseau complet : Mohamed Eladeb (groupe 4).

## Resultats (reseau complet, 5 images, 21 convolutions, 3,454 GMAC)
| Mode | Reseau (ms) | Produits matriciels (ms) |
|---|---|---|
| ARM 1 coeur (NEON) | 4 489 | 3 578 |
| ARM 2 coeurs | 2 842 | 1 933 |
| ARM + IP `gemm_tile` | 1 406 | 385 (8,9 GMAC/s) |

Acceleration de l'IP : x3,2 (reseau) et x9,3 (produits matriciels) face a 1 coeur ; x2,0 et x5,0 face a 2 coeurs.
Sorties identiques bit a bit entre les modes. Loi d'Amdahl : les produits matriciels representent 80 % du temps ARM seul (limite x4,9).
Ressources FPGA : 7 591 LUT (14,3 %), 14 119 registres, 38 BRAM (27,5 %), 135 DSP (61,4 %).

## Que lancer pour faire quoi ? (PowerShell)
| Je veux... | Commande |
|---|---|
| Preparer une carte SD et lancer la demo sur la Zybo | `carte\deployer_carte.ps1 -Images <dossier>` puis `carte\lire_resultats_carte.ps1` |
| Tout mesurer (JTAG, 3 modes dont 2 coeurs) + graphes + archive | `pipeline\pipeline_complet.ps1 -Port COM7` |
| Refaire seulement les graphes | `pipeline\pipeline_complet.ps1 -Port COM7 -Etapes figures,archive` |
| Recompiler le programme embarque | `xsct sw_sd\build_yolo_sd.tcl` (Vitis 2023.1) |

Voir `LISEZMOI.md` (arborescence detaillee) et `carte\LISEZMOI.md`.

## Arborescence
`sw_sd/` programme embarque (SD, NEON, 2 coeurs) | `sw_net/` poids `model.bin` | `sw/` test des couches | `vivado/`, `vivado_script/` materiel
`boot/BOOT.bin` demarrage autonome | `pipeline/` mesures -> graphes -> archive | `carte/` preparation de la carte SD

## Limites
Les chemins des scripts supposent `C:\Users\USER\Desktop\yolo_hw` (a adapter) ; carte Zybo **Z7-20** uniquement ; le mode 2 coeurs demande JTAG + Vitis ;
la precision de detection n'est pas evaluee sur un jeu annote (seule l'equivalence de calcul l'est). Le modele provient de YOLOv4-tiny (Darknet, COCO) : voir sa licence avant toute redistribution.
'@
Set-Content "README.md" $readme -Encoding UTF8

# ---- depot
if (-not (Test-Path ".git")) { git init -b main | Out-Null }
git rm -r --cached . -q 2>$null
git add -A
$gros = git ls-files | ForEach-Object { Get-Item $_ } | Where-Object { $_.Length -gt 50MB }
if ($gros) { Write-Host "ATTENTION : fichiers > 50 Mo (GitHub les refuse au-dela de 100 Mo) :"; $gros | ForEach-Object { Write-Host "  $($_.FullName)" }; Fail "retirez-les ou ajoutez-les a .gitignore" }
Write-Host "`nFichiers qui seront publies :" -ForegroundColor Cyan
git ls-files | ForEach-Object { $s = (Get-Item $_).Length; "{0,10:N0}  {1}" -f $s, $_ } | Select-Object -First 200
Write-Host ("`nTotal : {0} fichiers, {1:N1} Mo" -f (git ls-files).Count, ((git ls-files | ForEach-Object { (Get-Item $_).Length } | Measure-Object -Sum).Sum/1MB))

if (-not $Pousser) { Write-Host "`nApercu seulement. Relancez avec -Pousser pour committer et pousser (depot PRIVE par defaut)." -ForegroundColor Green; exit 0 }

$nomGit = git config user.name; $mail = git config user.email
if (-not $nomGit -or -not $mail) { Fail "identité Git absente : git config --global user.name `"Votre Nom`" ; git config --global user.email `"vous@exemple.com`"" }
$msg = "YOLOv4-tiny int8 sur Zybo Z7-20 : ARM (1 et 2 coeurs) vs IP gemm_tile, pipeline de mesure et kit de reproduction"
git rev-parse HEAD 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { git commit --amend -m $msg | Out-Null } else { git commit -m $msg | Out-Null }
$vis = if ($Public) { "--public" } else { "--private" }
if (Get-Command gh -ErrorAction SilentlyContinue) {
  gh auth status 2>$null
  if ($LASTEXITCODE -eq 0) {
    gh repo create $Nom $vis --source . --remote origin --push
    if ($LASTEXITCODE -eq 0) { Write-Host "`nPublie : $(gh repo view --json url -q .url)" -ForegroundColor Green; exit 0 }
  } else { Write-Host "gh n'est pas connecte (gh auth login)." -ForegroundColor Yellow }
}
if ($Compte) {
  git remote remove origin 2>$null
  git remote add origin "https://github.com/$Compte/$Nom.git"
  Write-Host "Depot attendu : https://github.com/$Compte/$Nom (vide, cree sur https://github.com/new)"
  git push -u origin main
} else {
  Write-Host "Commit fait. Relancez avec -Compte <votre pseudo GitHub> (apres avoir cree un depot VIDE '$Nom' sur https://github.com/new), ou installez gh (winget install GitHub.cli ; gh auth login)." -ForegroundColor Yellow
}
