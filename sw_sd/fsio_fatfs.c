/* fsio_fatfs.c - fsio sur la carte SD de la Zybo (bibliotheque xilffs / FatFs) */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "ff.h"
#include "fsio.h"
void led_poll(void);
#define CHUNK (256u * 1024u)
static FATFS fatfs;
int fs_mount(void) { return f_mount(&fatfs, "0:/", 1) == FR_OK ? 0 : -1; }
int fs_unmount(void) { return f_mount(NULL, "0:/", 0) == FR_OK ? 0 : -1; }
int fs_read(const char *p, void *b, size_t max, size_t *n)
{
    FIL f; if (f_open(&f, p, FA_READ) != FR_OK) return -1;
    if (f_size(&f) > max) { f_close(&f); return -2; }
    size_t tot = f_size(&f), done = 0; FRESULT r = FR_OK;
    while (done < tot && r == FR_OK) { UINT want = (UINT)((tot - done) < CHUNK ? (tot - done) : CHUNK), got = 0; r = f_read(&f, (char *)b + done, want, &got); done += got; led_poll(); if (got < want) break; }
    f_close(&f); *n = done; return (r == FR_OK && done == tot) ? 0 : -3;
}
int fs_write(const char *p, const void *b, size_t n)
{
    FIL f; if (f_open(&f, p, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    size_t done = 0; FRESULT r = FR_OK;
    while (done < n && r == FR_OK) { UINT want = (UINT)((n - done) < CHUNK ? (n - done) : CHUNK), w = 0; r = f_write(&f, (const char *)b + done, want, &w); done += w; led_poll(); if (w < want) break; }
    FRESULT c = f_close(&f); return (r == FR_OK && c == FR_OK && done == n) ? 0 : -2;
}
int fs_mkdir(const char *p) { FRESULT r = f_mkdir(p); return (r == FR_OK || r == FR_EXIST) ? 0 : -1; }
static int cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
int fs_list_images(const char *dir, char names[][64], int max)
{
    DIR d; FILINFO fi; int n = 0; if (f_opendir(&d, dir) != FR_OK) return -1;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        const char *x = strrchr(fi.fname, '.'); if (!x || strlen(fi.fname) > 63) continue;
        if (!strcasecmp(x, ".jpg") || !strcasecmp(x, ".jpeg") || !strcasecmp(x, ".png") || !strcasecmp(x, ".bmp")) strcpy(names[n++], fi.fname);
    }
    f_closedir(&d); qsort(names, n, 64, cmp); return n;
}
