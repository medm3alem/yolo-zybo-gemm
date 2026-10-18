#ifndef APP_SD_H
#define APP_SD_H
#include "yolo_core.h"
/* root : "0:/" sur la carte, "<dossier>/" sur PC. Lit model.bin et images/, ecrit dans out/ (sous root). */
/* hw_ip : produits matriciels sur le FPGA ; hw_neon / hw_scalar : sur l'ARM un coeur ; hw_neon2 : ARM deux coeurs (NULL si indisponible) */
int app_run(const yolo_hw_t *hw_ip, const yolo_hw_t *hw_neon, const yolo_hw_t *hw_scalar, const yolo_hw_t *hw_neon2, const char *root);
#endif
