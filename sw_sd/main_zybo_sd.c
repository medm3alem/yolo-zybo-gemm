/* main_zybo_sd.c - YOLOv4-tiny int8 sur Zybo : images lues sur la carte SD, resultats ecrits sur la carte SD. */
#include <stdio.h>
#include <string.h>
#include "xparameters.h"
#include "xgemm_tile.h"
#include "xtime_l.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "app_sd.h"
#include "gemm_arm.h"
#include "dualcore.h"

/* ---- LED verte LD4 (MIO7 du processeur) : clignote pendant le traitement, fixe quand la carte SD peut etre retiree ---- */
#define SLCR_LOCK     0xF8000004u
#define SLCR_UNLOCK   0xF8000008u
#define MIO_PIN_07    0xF800071Cu
#define GPIO_DATA0    0xE000A040u
#define GPIO_DIRM0    0xE000A204u
#define GPIO_OEN0     0xE000A208u
#define LED_BIT       (1u << 7)
static int led_mode = 0;                 /* 0 = eteinte, 1 = clignote lentement (travail), 2 = fixe (fini), 3 = clignote vite (erreur) */
static XTime led_last;
static void led_set(int on) { u32 d = Xil_In32(GPIO_DATA0); Xil_Out32(GPIO_DATA0, on ? (d | LED_BIT) : (d & ~LED_BIT)); }
static void led_init(void)
{
    Xil_Out32(SLCR_UNLOCK, 0xDF0D); Xil_Out32(MIO_PIN_07, 0x1600); Xil_Out32(SLCR_LOCK, 0x767B);   /* MIO7 = GPIO, sortie active */
    Xil_Out32(GPIO_DIRM0, Xil_In32(GPIO_DIRM0) | LED_BIT); Xil_Out32(GPIO_OEN0, Xil_In32(GPIO_OEN0) | LED_BIT);
    led_set(0); XTime_GetTime(&led_last);
}
void led_poll(void)                      /* a appeler souvent : fait basculer la LED sans interruption ni minuterie dediee */
{
    if (led_mode == 1 || led_mode == 3) {
        XTime t; XTime_GetTime(&t);
        XTime per = (XTime)COUNTS_PER_SECOND / (led_mode == 1 ? 4 : 10);                       /* 2 Hz ou 5 Hz */
        if (t - led_last >= per) { led_last = t; led_set(!(Xil_In32(GPIO_DATA0) & LED_BIT)); }
    }
}
static void led_mode_set(int m) { led_mode = m; if (m == 0) led_set(0); if (m == 2) led_set(1); }

int fs_mount(void);
int fs_read(const char *path, void *buf, size_t cap, size_t *n);
/* le mode neon2 n'est prepare que si config.txt le demande (evite toute attente du coeur 1 en demarrage autonome depuis la carte SD) */
static int want_neon2(const char *root)
{
    char path[64], buf[1024]; size_t n = 0;
    snprintf(path, sizeof path, "%sconfig.txt", root);
    if (fs_read(path, buf, sizeof buf - 1, &n) || n == 0) return 0;
    buf[n] = 0; return strstr(buf, "neon2") != NULL;
}
int fs_unmount(void);
static XGemm_tile ip;
static int g_timeout = 0;

static uint64_t now_us(void) { led_poll(); XTime t; XTime_GetTime(&t); return (uint64_t)((t * 1000000ULL) / COUNTS_PER_SECOND); }
static void do_flush(const void *p, size_t n) { Xil_DCacheFlushRange((UINTPTR)p, n); }
static void do_inval(const void *p, size_t n) { Xil_DCacheInvalidateRange((UINTPTR)p, n); }
static void no_cache(const void *p, size_t n) { (void)p; (void)n; }
static void gemm_ip(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    XGemm_tile_Set_Ap_r(&ip, (u64)(UINTPTR)Ap); XGemm_tile_Set_Bp(&ip, (u64)(UINTPTR)Bp); XGemm_tile_Set_C(&ip, (u64)(UINTPTR)Cp);
    XGemm_tile_Set_M(&ip, M); XGemm_tile_Set_N(&ip, N); XGemm_tile_Set_K(&ip, K);
    XTime t0, t1; XTime_GetTime(&t0);
    XGemm_tile_Start(&ip);
    while (!XGemm_tile_IsDone(&ip)) { led_poll(); XTime_GetTime(&t1); if ((t1 - t0) > (XTime)20 * COUNTS_PER_SECOND) { g_timeout = 1; return; } }
}

int main(void)
{
    led_init();
    XGemm_tile_Config *cfg = XGemm_tile_LookupConfig(XPAR_GEMM_TILE_0_DEVICE_ID);
    if (!cfg || XGemm_tile_CfgInitialize(&ip, cfg) != XST_SUCCESS) { printf("Erreur : init IP\r\n"); led_mode_set(3); for (;;) led_poll(); }
    led_mode_set(1);
    if (fs_mount()) { printf("Erreur : carte SD absente ou non FAT32\r\n"); led_mode_set(3); for (;;) led_poll(); }
    static const yolo_hw_t hw_ip     = { gemm_ip, now_us, do_flush, do_inval };
    static const yolo_hw_t hw_neon   = { gemm_neon_packed, now_us, no_cache, no_cache };     /* ARM seul : pas de DMA, donc pas de flush */
    static const yolo_hw_t hw_scalar = { gemm_scalar_packed, now_us, no_cache, no_cache };
    static const yolo_hw_t hw_neon2  = { gemm_dual_packed, now_us, no_cache, no_cache };      /* ARM 2 coeurs : coherence geree par dualcore.h */
    const yolo_hw_t *p_neon2 = NULL;
    if (want_neon2("0:/")) { if (dc_init()) p_neon2 = &hw_neon2; }              /* sonde seulement si config.txt contient neon2 */
    int r = app_run(&hw_ip, &hw_neon, &hw_scalar, p_neon2, "0:/");
    if (p_neon2) printf("Deux coeurs : %lu produits envoyes au coeur 1, %lu repasses sur un coeur (strategie S%lu)\r\n", (unsigned long)dc_calls, (unsigned long)dc_fallbacks, (unsigned long)dc_strategy);
    if (p_neon2 && dc_fallbacks) printf("ATTENTION : le coeur 1 a cesse de repondre : le mode neon2 n'est pas valide\r\n");
    if (g_timeout) printf("ATTENTION : timeout de l'IP pendant le traitement\r\n");
    fs_unmount();                                                              /* tout est ecrit et ferme */
    if (r || g_timeout) { led_mode_set(3); printf("Termine avec erreur : LED clignotante rapide\r\n"); for (;;) led_poll(); }
    led_mode_set(2);
    printf("Carte SD libre : LED verte fixe, vous pouvez la retirer.\r\n");
    return 0;
}
