# lire_resultats.ps1 -Sd D:   -- affiche le resume et le tableau de comparaison ecrits par la Zybo dans out\ de la carte SD
param([Parameter(Mandatory=$true)][string]$Sd)
$out = "$Sd\out"
if (-not (Test-Path $out)) { Write-Host "Dossier introuvable : $out"; exit 1 }
if (Test-Path "$out\resume.txt") { Write-Host "=== resume.txt ==="; Get-Content "$out\resume.txt" -Encoding UTF8 }
if (Test-Path "$out\benchmark.csv") {
    Write-Host "`n=== benchmark.csv (une ligne par image et par mode) ==="
    Import-Csv "$out\benchmark.csv" | Format-Table image, mode, repetitions, reseau_moy_ms, reseau_ecart_ms, quant_im2col_ms, produits_matriciels_ms, post_ms, sorties_identiques_au_premier_mode -AutoSize
}
# copie locale pour le rapport
$dest = "C:\Users\USER\Desktop\yolo_hw\logs\resultats_" + (Get-Date -Format "yyyyMMdd_HHmmss")
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$out\*" $dest -Recurse -Force
Write-Host "`nCopie : $dest"
