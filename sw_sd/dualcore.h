/* dualcore.h - produit matriciel int8 sur DEUX coeurs ARM (cote coeur 0). Inclus par main_zybo_sd.c.
 *
 * Principe : le coeur 1 execute un programme separe (gemm_cpu1, charge en OCM par le script XSCT) qui attend un travail dans une
 * zone partagee en DDR (0x1F000000), meme protocole que la Q2 (go / done / ready). Ici le travail est le produit aux formats
 * compactes de l'IP (noyau "4") : le coeur 0 calcule les colonnes-tuiles nt dans [0, n0), le coeur 1 celles dans [n0, NTL).
 * Les tuiles de C sont disjointes : aucun conflit d'ecriture. Les deux coeurs lisent les memes Ap et Bp.
 *
 * Coherence des caches : quatre strategies comme en Q2 (S3 = coherence materielle par le SCU, aucune maintenance ; S0..S2 =
 * maintenance par adresse). Au demarrage, un petit produit est calcule sur deux coeurs et compare a la version scalaire :
 * la premiere strategie EXACTE est retenue (S3 en premier). Si aucune n'est exacte, ou si le coeur 1 ne repond pas, le mode
 * "neon2" est indisponible (le programme continue sans lui). En cours de route, un blocage du coeur 1 fait repasser le calcul
 * sur un coeur (compte et signale) : le traitement des images n'est jamais bloque.
 */
#ifndef DUALCORE_H
#define DUALCORE_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "xil_types.h"
#include "xil_cache.h"
#include "xtime_l.h"
#include "gemm_arm.h"

#define DC_SHARED_ADDR 0x1F000000u
#define DC_KERNEL_PACKED 4u            /* produit aux formats de l'IP, partage par colonnes-tuiles */
typedef struct {                       /* meme disposition que shared_job_t du programme du coeur 1 */
    volatile u32 go, done, ready;
    u32 M, N, K;
    u32 row_start, row_end;            /* ici : plage de colonnes-tuiles [nt0, nt1) pour le coeur 1 */
    float alpha;
    void *A, *B, *C;
    u32 kernel, kc;
    u32 c1_sctlr, c1_actlr, c1_ttbr0, c1_mpidr;
} dc_job_t;
#ifndef DC_SH
#define DC_SH ((volatile dc_job_t *)DC_SHARED_ADDR)
#endif

#define DC_L2_BASE        0xF8F02000u
#define DC_L2_CACHE_SYNC  (DC_L2_BASE + 0x730u)
#define DC_L2_CLNINV_PA   (DC_L2_BASE + 0x7F0u)

extern void led_poll(void);
static u32 dc_strategy = 3, dc_pct = 50;           /* dc_pct : part des colonnes-tuiles calculee par le coeur 0 (%) */
static int dc_dead = 0, dc_ok = 0;
static u32 dc_calls = 0, dc_fallbacks = 0;

static void dc_l2_clean_inv(const void *p, size_t len)
{
    u32 a = (u32)(uintptr_t)p & ~31u, end = (u32)(uintptr_t)p + (u32)len;
    for (; a < end; a += 32u) *(volatile u32 *)DC_L2_CLNINV_PA = a;
    *(volatile u32 *)DC_L2_CACHE_SYNC = 0;
    __asm__ volatile ("dsb" ::: "memory");
}
static void dc_push(const void *p, size_t len)      /* ecriture coeur 0 -> visible du coeur 1 */
{
    switch (dc_strategy) {
    case 1:  Xil_DCacheFlush(); break;
    case 2:  Xil_DCacheFlushRange((INTPTR)p, (u32)len); dc_l2_clean_inv(p, len); break;
    case 3:  __asm__ volatile ("dsb" ::: "memory"); break;
    default: Xil_DCacheFlushRange((INTPTR)p, (u32)len); break;
    }
}
static void dc_pull(const void *p, size_t len)      /* ecriture coeur 1 -> visible du coeur 0 */
{
    switch (dc_strategy) {
    case 1:  Xil_DCacheFlush(); break;
    case 2:  dc_l2_clean_inv(p, len); Xil_DCacheInvalidateRange((INTPTR)p, (u32)len); break;
    case 3:  __asm__ volatile ("dsb" ::: "memory"); break;
    default: Xil_DCacheInvalidateRange((INTPTR)p, (u32)len); break;
    }
}
static inline void dc_sh_publish(const volatile void *p, size_t n)
{ if (dc_strategy == 3u) __asm__ volatile ("dsb" ::: "memory"); else Xil_DCacheFlushRange((INTPTR)p, (u32)n); }
static inline void dc_sh_refresh(const volatile void *p, size_t n)
{ if (dc_strategy == 3u) __asm__ volatile ("dmb" ::: "memory"); else Xil_DCacheInvalidateRange((INTPTR)p, (u32)n); }

/* attend qu'un drapeau devienne non nul, au plus `seconds` secondes ; 0 = ok, -1 = delai depasse */
static int dc_wait_flag(volatile u32 *f, int seconds)
{
    XTime t0, t; XTime_GetTime(&t0); u32 n = 0;
    dc_sh_refresh(f, sizeof(u32));
    while (*f == 0) {
        if ((++n & 0xFFFFu) == 0) { XTime_GetTime(&t); if ((t - t0) > (XTime)seconds * COUNTS_PER_SECOND) return -1; led_poll(); }
        dc_sh_refresh(f, sizeof(u32));
    }
    return 0;
}

/* nombre de colonnes-tuiles pour le coeur 0 : dc_pct % de NTL, au moins 1, et le coeur 1 en garde au moins 1 */
static int dc_split(int ntl)
{
    int n0 = (int)(((u32)ntl * dc_pct + 50u) / 100u);
    if (n0 < 1) n0 = 1;
    if (n0 > ntl - 1) n0 = ntl - 1;
    return n0;
}

/* 0 = ok ; -1 = le coeur 1 ne repond pas (il n'est alors plus utilise) */
static int dc_run_packed(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    const int MT = (M + 7) / 8, NTL = (N + 15) / 16, n0 = dc_split(NTL);
    dc_calls++;
    if (dc_wait_flag(&DC_SH->ready, 5)) { dc_dead = 1; return -1; }
    DC_SH->M = (u32)M; DC_SH->N = (u32)N; DC_SH->K = (u32)K; DC_SH->alpha = 1.0f;
    DC_SH->A = (void *)Ap; DC_SH->B = (void *)Bp; DC_SH->C = Cp;
    DC_SH->row_start = (u32)n0; DC_SH->row_end = (u32)NTL;
    DC_SH->kernel = DC_KERNEL_PACKED | (dc_strategy == 3u ? 0x100u : 0u); DC_SH->kc = 0;
    DC_SH->done = 0;
    dc_sh_publish(DC_SH, sizeof(dc_job_t));
    dc_push(Ap, (size_t)MT * K * 8);                                                       /* le coeur 1 lit tout A */
    dc_push(Bp + (size_t)n0 * K * 16, (size_t)(NTL - n0) * K * 16);                       /* ... et ses colonnes-tuiles de B */
    dc_push(Cp + (size_t)n0 * MT * 128, (size_t)(NTL - n0) * MT * 128 * 4u);              /* ... et ses tuiles de C */
    DC_SH->go = 1;
    dc_sh_publish(&DC_SH->go, sizeof(u32));

    gemm_neon_packed_range(M, N, K, Ap, Bp, Cp, 0, n0);                                    /* le coeur 0 calcule sa part en parallele */

    if (dc_wait_flag(&DC_SH->done, 20)) { dc_dead = 1; return -1; }
    dc_pull(Cp + (size_t)n0 * MT * 128, (size_t)(NTL - n0) * MT * 128 * 4u);
    DC_SH->go = 0;
    dc_sh_publish(&DC_SH->go, sizeof(u32));
    return 0;
}

/* appelee par yolo_core a la place du produit matriciel d'un seul coeur */
static void gemm_dual_packed(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    if (!dc_dead && dc_ok && (N + 15) / 16 >= 2 && dc_run_packed(M, N, K, Ap, Bp, Cp) == 0) return;
    dc_fallbacks++;                                                                         /* coeur 1 indisponible : un seul coeur */
    gemm_neon_packed(M, N, K, Ap, Bp, Cp);
}

#ifndef DC_DIAG
#define DC_DIAG ((volatile u32 *)(DC_SHARED_ADDR + 0x80u))     /* core_diag_t ecrit par le coeur 1 : mot 16 = etat, 17 = travaux termines, 4..6 = SCTLR/ACTLR/TTBR0 du coeur 1 */
#endif
static void dc_print_diag(void)
{
    u32 s0, a0; __asm__ volatile ("mrc p15, 0, %0, c1, c0, 0" : "=r" (s0)); __asm__ volatile ("mrc p15, 0, %0, c1, c0, 1" : "=r" (a0));
    volatile u32 *sh = (volatile u32 *)DC_SH, *dg = DC_DIAG;
    printf("# diag coeur 0 : SCTLR=0x%08lx (MMU=%lu D=%lu I=%lu) ACTLR=0x%08lx (SMP=%lu) SCU=0x%08lx\r\n", (unsigned long)s0, (unsigned long)(s0 & 1u),
           (unsigned long)((s0 >> 2) & 1u), (unsigned long)((s0 >> 12) & 1u), (unsigned long)a0, (unsigned long)((a0 >> 6) & 1u), (unsigned long)*(volatile u32 *)0xF8F00000u);
    printf("# diag zone partagee, vue du coeur 0 : go=%lu done=%lu ready=%lu | coeur 1 : etat=%lu travaux=%lu SCTLR=0x%08lx ACTLR=0x%08lx TTBR0=0x%08lx\r\n",
           (unsigned long)sh[0], (unsigned long)sh[1], (unsigned long)sh[2], (unsigned long)dg[16], (unsigned long)dg[17], (unsigned long)dg[4], (unsigned long)dg[5], (unsigned long)dg[6]);
    Xil_DCacheInvalidateRange((INTPTR)sh, 128); Xil_DCacheInvalidateRange((INTPTR)dg, 128);
    printf("# diag zone partagee, apres invalidation du cache L1 : go=%lu done=%lu ready=%lu | coeur 1 : etat=%lu travaux=%lu SCTLR=0x%08lx\r\n",
           (unsigned long)sh[0], (unsigned long)sh[1], (unsigned long)sh[2], (unsigned long)dg[16], (unsigned long)dg[17], (unsigned long)dg[4]);
}

/* le coeur 1 annonce "ready" : on le cherche d'abord en lecture simple (S3), puis avec invalidation du cache L1 (S0), puis avec L2 + L1 */
static int dc_wait_ready_any(void)
{
    dc_strategy = 3u; if (dc_wait_flag(&DC_SH->ready, 3) == 0) { printf("# deux coeurs : coeur 1 vu (lecture simple)\r\n"); return 1; }
    dc_strategy = 0u; if (dc_wait_flag(&DC_SH->ready, 2) == 0) { printf("# deux coeurs : coeur 1 vu seulement apres invalidation du cache L1\r\n"); return 1; }
    { XTime t0, t; XTime_GetTime(&t0);
      for (;;) { dc_l2_clean_inv((const void *)DC_SH, 64); Xil_DCacheInvalidateRange((INTPTR)DC_SH, 64);
                 if (DC_SH->ready) { printf("# deux coeurs : coeur 1 vu seulement apres nettoyage L2 + L1\r\n"); dc_strategy = 2u; return 1; }
                 XTime_GetTime(&t); if ((t - t0) > (XTime)2 * COUNTS_PER_SECOND) break; led_poll(); } }
    return 0;
}

/* Sonde au demarrage. Renvoie 1 si le mode deux coeurs est utilisable (strategie retenue dans dc_strategy). */
static int dc_init(void)
{
    static int8_t  pa[3 * 64 * 8] __attribute__((aligned(32)));
    static int8_t  pb[4 * 64 * 16] __attribute__((aligned(32)));
    static int32_t pc[4 * 3 * 128] __attribute__((aligned(32))), pr[4 * 3 * 128] __attribute__((aligned(32)));
    const int M = 24, N = 64, K = 64;
    uint32_t s = 12345u;
    for (size_t i = 0; i < sizeof pa; i++) { s = s * 1664525u + 1013904223u; pa[i] = (int8_t)((int)((s >> 24) % 255u) - 127); }
    for (size_t i = 0; i < sizeof pb; i++) { s = s * 1664525u + 1013904223u; pb[i] = (int8_t)((int)((s >> 24) % 255u) - 127); }
    gemm_scalar_packed(M, N, K, pa, pb, pr);
    dc_pct = 50; dc_ok = 0; dc_dead = 0;
    if (!dc_wait_ready_any()) { printf("# deux coeurs : le coeur 1 ne repond pas (programme gemm_cpu1 non charge ou en panne ?)\r\n"); dc_print_diag(); return 0; }
    static const u32 ordre[4] = { 3u, 0u, 1u, 2u };
    for (int i = 0; i < 4 && !dc_dead; i++) {
        dc_strategy = ordre[i]; u32 worst = 0;
        for (int rep = 0; rep < 3; rep++) {
            memset(pc, 0, sizeof pc); Xil_DCacheFlushRange((INTPTR)pc, sizeof pc);
            u32 bad = (u32)(sizeof pc / 4);
            if (dc_run_packed(M, N, K, pa, pb, pc) == 0) { bad = 0; for (size_t j = 0; j < sizeof pc / 4; j++) if (pc[j] != pr[j]) bad++; }
            if (bad > worst) worst = bad;
        }
        printf("# deux coeurs : strategie S%lu, erreurs %lu/%lu\r\n", (unsigned long)dc_strategy, (unsigned long)worst, (unsigned long)(sizeof pc / 4));
        if (worst == 0) { dc_ok = 1; break; }
    }
    if (!dc_ok) printf("# deux coeurs : aucune strategie de coherence exacte -> mode neon2 indisponible\r\n");
    return dc_ok;
}
#endif
