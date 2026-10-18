# pipeline : mesures -> graphes -> archive
Lancer : `powershell -ExecutionPolicy Bypass -File .\pipeline_complet.ps1 -Port COM7 [-Etapes ...]`
Etapes (separees par des virgules) : `couches`, `reseau`, `sd`, `vivado`, `figures`, `archive`.
- `sd` : benchmark sur les images de la carte (ARM 1 coeur, ARM 2 coeurs, ARM + IP), environ 5 minutes.
- `figures` : appelle `make_figures.py` -> `..\rapport_figures\`.
- `archive` : cree `..\envoi_rapport_<date>.zip` (resultats + figures + chiffres_cles.txt).
Options : `-Sd D:` (lettre de la carte), `-Xsct <chemin>`, `-SansDeuxCoeurs`, `-Cpu1Elf <chemin>`.
`config_puissance\` : configurations de mesure d'energie (non utilisees par defaut).
