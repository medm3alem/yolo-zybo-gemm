# pipeline : mesures par JTAG → graphes → archive
Tout en une commande (carte Zybo en JTAG + USB série, carte SD avec vos images) :
`powershell -ExecutionPolicy Bypass -File .\pipeline_complet.ps1 -Port COM7`

Étapes (`-Etapes a,b,c`) : `couches`, `reseau`, `sd`, `vivado`, `figures`, `archive`.
| Étape | Effet |
|---|---|
| `sd` | benchmark sur les images de la carte : ARM 1 cœur, ARM 2 cœurs (si l'ELF du cœur 1 existe), ARM + IP (~5 min) |
| `figures` | `make_figures.py` → `..\rapport_figures\` (graphes + `chiffres_cles.txt`) |
| `archive` | `..\envoi_rapport_<date>.zip` (résultats + figures + chiffres) |

Options : `-Port COM7`, `-Sd D:`, `-Xsct <chemin>`, `-SansDeuxCoeurs`, `-Cpu1Elf <chemin>`.
Mode 2 cœurs : l'ELF du cœur 1 se compile depuis `cpu1\gemm_cpu1_full.c` (voir le kit `yolo_hw_2coeurs`) ; sans lui, les modes ARM 1 cœur et IP sont mesurés quand même.
`config_puissance\` : configurations de mesure d'énergie (non utilisées par défaut).
