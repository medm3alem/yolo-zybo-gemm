# carte : programme + images sur la carte SD, démarrage autonome de la Zybo

## En 3 étapes
1. Carte SD en **FAT32** dans le PC, vos images dans un dossier (jpg, jpeg, png, bmp ; 64 maximum).
2. `powershell -ExecutionPolicy Bypass -File .\deployer_carte.ps1 -Images "C:\mes\images"`
   (compile le programme si besoin, fabrique `BOOT.bin`, vide `out\`, copie images + `model.bin` + `config.txt`, éjecte la carte)
3. Carte dans la Zybo, cavalier **JP5 sur SD**, allumer. LED verte LD4 : clignote = calcul ; **fixe = fini** ; clignote vite = erreur.
   Puis remettre la carte dans le PC : `.\lire_resultats_carte.ps1`.

## Options de `deployer_carte.ps1`
| Option | Effet |
|---|---|
| `-Images <dossier>` | remplace le contenu de `images\` de la carte (sans elle : garde les images déjà sur la carte) |
| `-Config normal` (défaut) | IP seule, 1 passage ; écrit `.txt`, `.csv` et `_det.bmp` |
| `-Config production` | IP seule, 1 passage ; **n'écrit que `<image>_det.bmp`** dans `out\` |
| `-Config benchmark` | IP + ARM NEON, 5 passages (compare les temps, ~4-5 min pour 5 images) |
| `-Config complet` | ajoute l'ARM scalaire (très long) |
| `-Reconstruire` | recompile le programme et refabrique `BOOT.bin` |
| `-Sd D:` | si la carte n'est pas détectée automatiquement |
| `-SansEjecter` | ne pas éjecter la carte |

## Lecture des résultats
`lire_resultats_carte.ps1` : en benchmark/normal, affiche le résumé et copie `out\` dans `logs\resultats_<date>` ; en production, copie les images annotées dans `logs\production_<date>` et ouvre le dossier.

## À savoir
- Le mode **2 cœurs** n'existe pas en démarrage autonome (il demande le PC en JTAG) : voir `pipeline\`.
- Le script recompile seul si les sources de `sw_sd\` sont plus récentes que le programme.
- `model.bin` (6 Mo) est copié depuis `sw_net\` à chaque fois.
