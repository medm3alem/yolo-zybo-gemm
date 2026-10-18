# sw_sd : programme embarque (application SD)
- `main_zybo_sd.c`, `app_sd.c/.h`, `yolo_core.c/.h` : lecture des images SD, reseau YOLOv4-tiny int8, modes ip / neon / neon2
- `gemm_arm.h`, `gemm_pack.h` : noyau NEON et formats de panneaux de l'IP ; `dualcore.h` : protocole 2 coeurs (coeur 1)
- `config_sd\config_*.txt` : modes et repetitions (`config_2coeurs.txt` = ARM 1 coeur, 2 coeurs, IP)
- `build_yolo_sd.tcl` : compile (Vitis) ; `run_yolo_sd.tcl` / `run_yolo_sd_2c.tcl` : lance en JTAG (sans / avec coeur 1)
- `preparer_sd.ps1` : prepare la carte SD ; `lire_resultats.ps1` : recupere les resultats ; `make_boot.ps1` : BOOT.bin
- `diag_cpu1.tcl` : etat du coeur 1 en cas d'echec du mode 2 coeurs
Source du programme du coeur 1 : `C:\Users\USER\gemm_dualcore\gemm_cpu1\src\gemm_cpu1.c` (version `gemm_cpu1_full.c` du kit 2 coeurs).
