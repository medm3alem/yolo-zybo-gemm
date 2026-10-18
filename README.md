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
