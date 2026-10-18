/*
 * main.c - Test bare-metal de l'IP gemm_tile (Zybo Z7-20, 100 MHz)
 * C (en tuiles 8x16, voir gemm_pack.h) int32 = A[M][K] int8 x B[K][N] int8, pour les 21 couches de YOLOv4-tiny.
 * Compare bit a bit avec une reference ARM (int32), et mesure :
 *   ip_us   : start -> done de l'IP
 *   arm_us  : reference ARM scalaire (boucles i,k,j, -O2, 1 coeur)
 *   pack_us : mise en forme de B pour l'IP (a ajouter au temps IP dans le reseau reel)
 * Option de compilation : -DONLY_LAYER=18 pour ne tester qu'une couche (index dans le tableau).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "xparameters.h"
#include "xgemm_tile.h"
#include "xtime_l.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "gemm_pack.h"

#ifndef ONLY_LAYER
#define ONLY_LAYER (-1)
#endif
#define TIMEOUT_S 20

typedef struct { int id, M, K, N; } layer_t;
static const layer_t LAYERS[] = {
  {0,32,27,43264},{1,64,288,10816},{2,64,576,10816},{4,32,288,10816},{5,32,288,10816},
  {7,64,64,10816},{10,128,1152,2704},{12,64,576,2704},{13,64,576,2704},{15,128,128,2704},
  {18,256,2304,676},{20,128,1152,676},{21,128,1152,676},{23,256,256,676},{26,512,4608,169},
  {27,256,512,169},{28,512,2304,169},{29,255,512,169},{32,128,256,169},{35,256,3456,676},
  {36,255,256,676}
};
#define NL ((int)(sizeof(LAYERS)/sizeof(LAYERS[0])))

/* tailles max : couche 0 -> C = 5.5 Mo ; couche 2 -> B = 576 x 10816 = 6.2 Mo */
#define MAXA   (512*4608)
#define MAXB   (576*10816)           /* couche 2 : 6.2 Mo */
#define MAXBP  (576*10816 + 4608*16) /* B packe : N arrondi a 16 */
#define MAXAP  (MAXA + 8*4608)
#define MAXC   (32*43264)            /* C en tuiles : (M arrondi a 8) x (N arrondi a 16), max = couche 0 */
static int8_t  A [MAXA ] __attribute__((aligned(32)));
static int8_t  B [MAXB ] __attribute__((aligned(32)));
static int8_t  Ap[MAXAP] __attribute__((aligned(32)));
static int8_t  Bp[MAXBP] __attribute__((aligned(32)));
static int32_t Cip [MAXC] __attribute__((aligned(32)));
static int32_t Cref[MAXC] __attribute__((aligned(32)));

static XGemm_tile ip;

static unsigned us_of(XTime t) { return (unsigned)((t * 1000000ULL) / COUNTS_PER_SECOND); }
static uint32_t rng = 12345u;
static int8_t rnd8(void) { rng = rng * 1664525u + 1013904223u; return (int8_t)(rng >> 24); }

static int run_one(int M, int K, int N, int id)
{
    XTime t0, t1;
    long long macs = (long long)M * K * N;
    for (long i = 0; i < (long)M * K; i++) A[i] = rnd8();
    for (long i = 0; i < (long)K * N; i++) B[i] = rnd8();

    /* mise en forme */
    XTime_GetTime(&t0);
    pack_A(A, M, K, Ap);
    XTime_GetTime(&t1);
    unsigned packA_us = us_of(t1 - t0);
    XTime_GetTime(&t0);
    pack_B_fast(B, K, N, Bp);
    XTime_GetTime(&t1);
    unsigned packB_us = us_of(t1 - t0);

    size_t nA = gp_bytesA(M, K), nB = gp_bytesB(K, N);
    size_t nC = gp_bytesC(M, N);            /* C en tuiles, bords compris */
    size_t nRef = (size_t)M * N * sizeof(int32_t);
    XTime_GetTime(&t0);
    Xil_DCacheFlushRange((UINTPTR)Ap, nA);
    Xil_DCacheFlushRange((UINTPTR)Bp, nB);
    XTime_GetTime(&t1);
    unsigned flush_us = us_of(t1 - t0);          /* vidage du cache avant l'IP (A et B) */
    for (size_t i = 0; i < nC / 4; i++) Cip[i] = 0x7fffffff;      /* sentinelle */
    Xil_DCacheFlushRange((UINTPTR)Cip, nC);

    XGemm_tile_Set_Ap_r(&ip, (u64)(UINTPTR)Ap);
    XGemm_tile_Set_Bp  (&ip, (u64)(UINTPTR)Bp);
    XGemm_tile_Set_C   (&ip, (u64)(UINTPTR)Cip);
    XGemm_tile_Set_M(&ip, M);
    XGemm_tile_Set_N(&ip, N);
    XGemm_tile_Set_K(&ip, K);

    int timeout = 0;
    XTime_GetTime(&t0);
    XGemm_tile_Start(&ip);
    while (!XGemm_tile_IsDone(&ip)) {
        XTime_GetTime(&t1);
        if ((t1 - t0) > (XTime)TIMEOUT_S * COUNTS_PER_SECOND) { timeout = 1; break; }
    }
    XTime_GetTime(&t1);
    unsigned ip_us = us_of(t1 - t0);
    Xil_DCacheInvalidateRange((UINTPTR)Cip, nC);
    if (timeout) { xil_printf("L%d : TIMEOUT de l'IP\r\n", id); return 1; }
    u32 rc = XGemm_tile_Get_return(&ip);

    /* reference ARM : i,k,j (acces contigus) */
    XTime_GetTime(&t0);
    memset(Cref, 0, nRef);
    for (int i = 0; i < M; i++)
        for (int k = 0; k < K; k++) {
            int32_t a = A[(size_t)i * K + k];
            const int8_t *b = &B[(size_t)k * N];
            int32_t *c = &Cref[(size_t)i * N];
            for (int j = 0; j < N; j++) c[j] += a * b[j];
        }
    XTime_GetTime(&t1);
    unsigned arm_us = us_of(t1 - t0);

    long err = 0;
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++)
            if (Cip[gp_cidx(i, j, M)] != Cref[(size_t)i * N + j]) err++;

    unsigned mmacs_ip  = ip_us  ? (unsigned)(macs / ip_us)  : 0;   /* MAC/us = MMAC/s */
    unsigned mmacs_arm = arm_us ? (unsigned)(macs / arm_us) : 0;
    xil_printf("L%2d M=%3d K=%4d N=%5d | IP %7u us (%5u MMAC/s) | ARM %8u us (%4u MMAC/s) | packB %6u us packA %6u us | flush %5u us | rc=%d err=%d %s\r\n",
               id, M, K, N, ip_us, mmacs_ip, arm_us, mmacs_arm, packB_us, packA_us, flush_us, (int)rc, (int)err, err ? "ECHEC" : "ok");
    xil_printf("CSV,%d,%d,%d,%d,%u,%u,%u,%u,%d,%u\r\n", id, M, K, N, ip_us, arm_us, packB_us, packA_us, (int)err, flush_us);
    return err != 0;
}

int main(void)
{
    xil_printf("\r\n=== Test IP gemm_tile (int8 -> int32, M,N,K runtime) ===\r\n");
    XGemm_tile_Config *cfg = XGemm_tile_LookupConfig(XPAR_GEMM_TILE_0_DEVICE_ID);
    if (!cfg || XGemm_tile_CfgInitialize(&ip, cfg) != XST_SUCCESS) {
        xil_printf("Erreur : initialisation de l'IP\r\n"); return -1;
    }
    xil_printf("IP initialisee, idle=%d\r\n", (int)XGemm_tile_IsIdle(&ip));

    int bad = 0;
    /* tests de base d'abord : tuile exacte, puis bords */
    bad += run_one(8, 16, 16, -1);
    bad += run_one(9, 17, 17, -2);
    bad += run_one(32, 64, 100, -3);
    if (bad) { xil_printf("Tests de base en echec : arret.\r\n"); return 1; }

    for (int l = 0; l < NL; l++) {
        if (ONLY_LAYER >= 0 && LAYERS[l].id != ONLY_LAYER) continue;
        bad += run_one(LAYERS[l].M, LAYERS[l].K, LAYERS[l].N, LAYERS[l].id);
    }
    xil_printf(bad ? "BILAN : ECHEC (%d couches)\r\n" : "BILAN : TOUT OK\r\n", bad);
    xil_printf("=== Fin ===\r\n");
    return bad;
}
