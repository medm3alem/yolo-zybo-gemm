# yolo_hw : YOLOv4-tiny int8 sur Zybo Z7-20 (ARM seul / 2 coeurs / ARM + IP gemm_tile)

## Que lancer pour faire quoi ? (PowerShell, depuis `pipeline\`)
| Je veux... | Commande |
|---|---|
| **Tout refaire** : mesures sur la carte, graphes, archive pour le rapport | `cd pipeline; powershell -ExecutionPolicy Bypass -File .\pipeline_complet.ps1 -Port COM7` |
| Seulement le benchmark sur images (ARM 1 coeur, 2 coeurs, IP) + graphes + archive | `... .\pipeline_complet.ps1 -Port COM7 -Etapes sd,vivado,figures,archive` |
| Refaire seulement les graphes (sans carte) | `... .\pipeline_complet.ps1 -Port COM7 -Etapes figures,archive` |
| Recompiler le programme de la carte apres une modification de `sw_sd\` | `Remove-Item vitis_ws -Recurse -Force; & C:\Xilinx\Vitis\2023.1\bin\xsct.bat sw_sd\build_yolo_sd.tcl` |
| Diagnostiquer le coeur 1 apres un echec du mode 2 coeurs | `& C:\Xilinx\Vitis\2023.1\bin\xsct.bat sw_sd\diag_cpu1.tcl` |
| Fabriquer `BOOT.bin` (demarrage autonome, sans PC ; pas de mode 2 coeurs) | `powershell -ExecutionPolicy Bypass -File sw_sd\make_boot.ps1` |

Pre-requis : carte Zybo en JTAG + USB serie (COM7), carte SD (BOOTFS) avec vos images, Vitis 2023.1.
Le mode 2 coeurs exige l'ELF du coeur 1 : `C:\Users\USER\gemm_dualcore\gemm_cpu1\Debug\gemm_cpu1.elf`.

## Arborescence
- `pipeline\`        : script `pipeline_complet.ps1` (mesures -> graphes -> archive) et `make_figures.py`
- `sw_sd\`           : programme de la carte (lit les images sur SD, ARM/IP, 2 coeurs) + scripts xsct
- `sw_net\`          : `model.bin` (poids int8) et script `run_yolo.tcl` (reseau complet en JTAG)
- `sw\`              : test des couches une par une (`run_test.tcl`)
- `vivado\`, `vivado_script\` : projet / rapports Vivado (ressources FPGA)
- `vitis_ws\`        : espace Vitis genere (a supprimer puis reconstruire en cas de souci)
- `boot\`            : `BOOT.bin` (demarrage autonome)
- `logs\`            : journaux des mesures (les plus recents ; `logs\archives\` = anciens)
- `rapport_figures\` : graphes du rapport (generes)
- `envoi_rapport_*.zip` : derniere archive a envoyer / mettre dans le rapport
- `_a_supprimer\`    : ce qui a ete juge inutile ; supprimable quand vous etes sur
