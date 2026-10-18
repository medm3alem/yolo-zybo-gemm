/* fsio.h - acces fichiers portable : carte SD (FatFs) sur la Zybo, dossier sur PC */
#ifndef FSIO_H
#define FSIO_H
#include <stddef.h>
int fs_read(const char *path, void *buf, size_t max, size_t *n);        /* 0 = ok */
int fs_write(const char *path, const void *buf, size_t n);              /* 0 = ok */
int fs_mkdir(const char *path);                                         /* 0 = ok ou deja existant */
int fs_list_images(const char *dir, char names[][64], int max);         /* .jpg .jpeg .png .bmp, tries ; renvoie le nombre */
#endif
