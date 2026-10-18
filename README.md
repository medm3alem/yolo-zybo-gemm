# YOLOv4-tiny int8 sur Zybo Z7-20 : ARM Cortex-A9 vs IP FPGA `gemm_tile`

Projet CSC_5RO06_TA / ROB306 (ENSTA Paris), groupe 4. On accélère le produit matriciel (GEMM) de YOLOv4-tiny int8 sur la Zybo Z7-20 et on compare
l'ARM seul (NEON, 1 et 2 cœurs) à l'ARM + une IP FPGA `gemm_tile`. Partie ARM9 et application « réseau complet » : Mohamed Eladeb.

## Résultats (réseau complet, 5 images, 21 convolutions, 3,454 GMAC)
| Mode | Réseau (ms) | Produits matriciels (ms) |
|---|---|---|
| ARM 1 cœur (NEON) | 4 489 | 3 578 |
| ARM 2 cœurs | 2 842 | 1 933 |
| ARM + IP `gemm_tile` | 1 406 | 385 (8,9 GMAC/s) |

Accélération de l'IP : ×3,2 (réseau) et ×9,3 (produits matriciels) face à 1 cœur ; ×2,0 et ×5,0 face à 2 cœurs. Sorties identiques bit à bit entre les modes.
Loi d'Amdahl : les produits matriciels font 80 % du temps ARM seul (limite ×4,9). Sans le mode 2 cœurs actif, l'IP mesure 1 388 ms (×3,23).
FPGA : 7 591 LUT (14,3 %), 14 119 registres (13,3 %), 38 BRAM (27,5 %), 135 DSP (61,4 %).

## Que lancer pour faire quoi ? (PowerShell)
| Je veux… | Commande |
|---|---|
| **Mettre mes images sur la carte SD et lancer la Zybo seule** (compile et fabrique `BOOT.bin` si besoin) | `carte\deployer_carte.ps1 -Images <dossier>` puis, après le calcul, `carte\lire_resultats_carte.ps1` |
| …en **mode production** (seulement les images annotées en sortie) | `carte\deployer_carte.ps1 -Config production -Images <dossier>` |
| …en **benchmark** (IP + ARM, 5 passages, tableau des temps) | `carte\deployer_carte.ps1 -Config benchmark` |
| **Tout mesurer** par JTAG (ARM 1 cœur, 2 cœurs, IP) + graphes + archive | `pipeline\pipeline_complet.ps1 -Port COM7` |
| Refaire seulement les graphes | `pipeline\pipeline_complet.ps1 -Port COM7 -Etapes figures,archive` |
| Recompiler le programme embarqué | `xsct sw_sd\build_yolo_sd.tcl` (Vitis 2023.1) |
| Diagnostiquer le cœur 1 si le mode 2 cœurs échoue | `xsct sw_sd\diag_cpu1.tcl` |

Prérequis : Zybo **Z7-20**, carte SD FAT32, Vitis 2023.1 (compilation et JTAG), PowerShell. Sans Vitis, utilisez le **kit de reproduction** (`BOOT.bin` fourni).

## Arborescence
| Dossier | Contenu |
|---|---|
| `carte/` | préparation de la carte SD (images, `BOOT.bin`, config) et lecture des résultats |
| `pipeline/` | mesures par JTAG → graphes → archive (`pipeline_complet.ps1`, `make_figures.py`) |
| `sw_sd/` | programme embarqué (lecture SD, YOLOv4-tiny int8, ARM / IP / 2 cœurs) et scripts xsct |
| `sw_net/` | poids `model.bin`, description du réseau, scripts de référence (numpy) |
| `sw/` | test des couches une par une |
| `vivado/`, `vivado_script/` | description matérielle (`.xsa`) et rapport de ressources |
| `boot/` | `BOOT.bin` pour démarrage autonome |
| `rapport_figures/` | graphes du rapport (générés) |

## Modes de la carte (`config.txt`)
`normal` (IP, 1 passage) · `benchmark` (IP + ARM NEON, 5 passages) · `complet` (+ ARM scalaire) · `production` (IP, 1 passage, **seule l'image `_det.bmp` est écrite**).
Le mode 2 cœurs ne fonctionne que par JTAG (le cœur 1 est chargé depuis le PC) : il se mesure avec `pipeline\pipeline_complet.ps1`.

## Limites
- Les scripts supposent `C:\Users\USER\Desktop\yolo_hw` (à adapter) et Vitis dans `C:\Xilinx\Vitis\2023.1`.
- Le noyau 2 cœurs du réseau (formats de l'IP) est plus lent que celui de la Q2 (1 933 ms contre 1 386 ms sur les produits matriciels).
- La précision de détection n'est pas évaluée sur un jeu annoté : seule l'équivalence de calcul entre les modes l'est.
- Le modèle dérive de YOLOv4-tiny (Darknet, COCO) ; les poids `.weights` ne sont pas dans le dépôt : voir leur licence avant redistribution.
