# sw_sd : programme embarqué (application SD)
Lit `model.bin` et les images de la carte SD, exécute YOLOv4-tiny int8 (quantification, im2col, produit matriciel, post-traitement), décode les boîtes (seuil 0,25, NMS 0,45) et écrit les résultats dans `out\`.

| Fichier | Rôle |
|---|---|
| `main_zybo_sd.c`, `app_sd.c/.h` | point d'entrée, boucle sur les images, modes (`ip`, `neon`, `neon2`, `scalar`), config, écriture des sorties |
| `yolo_core.c/.h` | réseau YOLOv4-tiny int8 (21 convolutions) |
| `gemm_arm.h`, `gemm_pack.h` | noyau NEON et formats de panneaux de l'IP |
| `dualcore.h` | protocole 2 cœurs (cœur 1 chargé par JTAG) et choix de la stratégie de cohérence de cache |
| `fsio*.h/.c`, `stb_image.h`, `font8x12.h` | fichiers (FatFs), décodage d'images, texte des étiquettes |
| `config_sd/config_*.txt` | `normal`, `benchmark`, `complet_avec_scalaire`, `2coeurs`, `production` |
| `build_yolo_sd.tcl` | compile (Vitis 2023.1) : plateforme, FatFs, application |
| `run_yolo_sd.tcl`, `run_yolo_sd_2c.tcl` | lance par JTAG (sans / avec cœur 1) |
| `make_boot.ps1` | fabrique `boot\BOOT.bin` (FSBL + bitstream + programme) |
| `lire_resultats.ps1`, `diag_cpu1.tcl` | lecture des résultats ; état du cœur 1 en cas d'échec |

## `config.txt` (à la racine de la carte)
`modes=ip,neon,neon2,scalar` · `repeat=5` · `diag=0` · `production=1` (force `modes=ip`, 1 passage, n'écrit que `<image>_det.bmp`).

Code du cœur 1 (mode 2 cœurs) : projet séparé `gemm_cpu1` (voir `pipeline\`).
