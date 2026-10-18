# lancer_figures.ps1 -- installe matplotlib/numpy si besoin, puis genere les figures dans yolo_hw\rapport_figures
$ROOT = "C:\Users\USER\Desktop\yolo_hw"
python -c "import matplotlib, numpy" 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host "Installation de matplotlib et numpy..."; python -m pip install matplotlib numpy }
python "$PSScriptRoot\make_figures.py" --root $ROOT @args
Write-Host "`nFigures : $ROOT\rapport_figures  (PDF pour LaTeX, PNG pour apercu, chiffres_cles.txt)"
