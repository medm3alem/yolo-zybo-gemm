# carte : programme + images sur la carte SD, puis demarrage autonome de la Zybo

## En 3 etapes
1. Carte SD en **FAT32** dans le PC, vos images dans un dossier (jpg, jpeg, png, bmp ; 64 maximum).
2. `powershell -ExecutionPolicy Bypass -File .\deployer_carte.ps1 -Images "C:\chemin\vers\mes_images"`
   (compile le programme si besoin, fabrique `BOOT.bin`, vide `out\`, copie images + `model.bin` + `config.txt`, ejecte la carte)
3. Carte dans la Zybo, cavalier **JP5 sur SD**, allumer. LED verte LD4 : clignote = calcul ; **fixe = fini** ; clignote vite = erreur.
   Puis remettre la carte dans le PC : `.\lire_resultats_carte.ps1` (resume + copie dans `logs\`). Les detections sont dans `out\` (`<image>_det.bmp`, `<image>.txt`, `resultats.csv`, `benchmark.csv`).

## Options de deployer_carte.ps1
| Option | Effet |
|---|---|
| `-Images <dossier>` | remplace le contenu de `images\` de la carte (sans cette option, garde les images deja sur la carte) |
| `-Config normal` (defaut) | IP seule, 1 passage par image |
| `-Config benchmark` | IP + ARM NEON, 5 passages (compare les temps) |
| `-Config complet` | ajoute l'ARM scalaire (tres long) |
| `-Reconstruire` | recompile le programme et refabrique `BOOT.bin` |
| `-Sd D:` | si la carte n'est pas detectee automatiquement |
| `-SansEjecter` | ne pas ejecter la carte |

## A savoir
- Le mode **2 coeurs** n'existe pas en demarrage autonome (il demande le PC et le JTAG) : il se mesure avec `pipeline\pipeline_complet.ps1`.
- Le script reconstruit tout seul le programme si les sources de `sw_sd\` sont plus recentes que l'ELF.
- Le modele (`model.bin`, 6 Mo) est copie a chaque fois depuis `sw_net\`.
