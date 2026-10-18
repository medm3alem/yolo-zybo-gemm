/* gemm_arm.h - produits matriciels int8 -> int32 sur le processeur ARM, aux MEMES formats compactes que l'IP :
 *   Ap[mt][k][8] (int8), Bp[nt][k][16] (int8), C en tuiles : mot ((nt*MT + mt)*128 + i*16 + j), i = ligne dans la tuile (0..7), j = colonne (0..15)
 * Les resultats sont identiques a l'IP (arithmetique entiere exacte, valeurs dans [-127,127]).
 */
#ifndef GEMM_ARM_H
#define GEMM_ARM_H
#include <stdint.h>
#include <string.h>

/* version scalaire C, de reference. Les fonctions *_range calculent seulement les colonnes-tuiles nt dans [nt0, nt1) :
 * les tuiles de C de deux plages differentes sont disjointes (deux coeurs peuvent donc travailler en parallele sans conflit). */
static void gemm_scalar_packed_range(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp, int nt0, int nt1)
{
    const int MT = (M + 7) / 8; (void)N;
    for (int nt = nt0; nt < nt1; nt++) for (int mt = 0; mt < MT; mt++) {
        int32_t acc[8][16]; memset(acc, 0, sizeof acc);
        const int8_t *a = Ap + (size_t)mt * K * 8, *b = Bp + (size_t)nt * K * 16;
        for (int k = 0; k < K; k++, a += 8, b += 16)
            for (int i = 0; i < 8; i++) { const int ai = a[i]; for (int j = 0; j < 16; j++) acc[i][j] += ai * b[j]; }
        memcpy(Cp + ((size_t)nt * MT + mt) * 128, acc, sizeof acc);
    }
}
static void gemm_scalar_packed(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    gemm_scalar_packed_range(M, N, K, Ap, Bp, Cp, 0, (N + 15) / 16);
}

/* si le projet est compile avec -mfpu=vfpv3 (Vitis), on active NEON pour ce fichier seulement */
#if defined(__arm__) && !defined(__ARM_NEON)
#pragma GCC push_options
#pragma GCC target("fpu=neon")
#define GEMM_FORCE_NEON 1
#define GEMM_POP_OPTIONS 1
#endif
#if defined(__ARM_NEON) || defined(GEMM_FORCE_NEON)
#include <arm_neon.h>
#define GEMM_HAVE_NEON 1
/* micro-noyau : 4 lignes x 8 colonnes, accumulateurs int32 en registres, pas de debordement :
 * K <= 4608 et |a*b| <= 127*127 -> |somme| < 7.5e7 < 2^31 */
#define MK4x8(R0, CN, SEL)                                                                                       \
    do {                                                                                                      \
        int32x4_t c00 = vdupq_n_s32(0), c01 = c00, c10 = c00, c11 = c00, c20 = c00, c21 = c00, c30 = c00, c31 = c00; \
        const int8_t *a = ap, *b = bp + (CN);                                                           \
        for (int k = 0; k < K; k++, a += 8, b += 16) {                                                         \
            const int16x8_t bb = vmovl_s8(vld1_s8(b));                                                         \
            const int16x4_t aa = SEL(vmovl_s8(vld1_s8(a)));  /* 8 lignes chargees, on garde les 4 voulues (R0 = 0 ou 4) */ \
            const int16x4_t b0 = vget_low_s16(bb), b1 = vget_high_s16(bb);                                     \
            c00 = vmlal_lane_s16(c00, b0, aa, 0); c01 = vmlal_lane_s16(c01, b1, aa, 0);                        \
            c10 = vmlal_lane_s16(c10, b0, aa, 1); c11 = vmlal_lane_s16(c11, b1, aa, 1);                        \
            c20 = vmlal_lane_s16(c20, b0, aa, 2); c21 = vmlal_lane_s16(c21, b1, aa, 2);                        \
            c30 = vmlal_lane_s16(c30, b0, aa, 3); c31 = vmlal_lane_s16(c31, b1, aa, 3);                        \
        }                                                                                                      \
        int32_t *c = ct + (R0) * 16 + (CN);                                                                    \
        vst1q_s32(c, c00); vst1q_s32(c + 4, c01); vst1q_s32(c + 16, c10); vst1q_s32(c + 20, c11);              \
        vst1q_s32(c + 32, c20); vst1q_s32(c + 36, c21); vst1q_s32(c + 48, c30); vst1q_s32(c + 52, c31);        \
    } while (0)

static void gemm_neon_packed_range(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp, int nt0, int nt1)
{
    const int MT = (M + 7) / 8; (void)N;
    for (int nt = nt0; nt < nt1; nt++) {
        const int8_t *bpn = Bp + (size_t)nt * K * 16;
        for (int mt = 0; mt < MT; mt++) {
            const int8_t *ap = Ap + (size_t)mt * K * 8, *bp = bpn;
            int32_t *ct = Cp + ((size_t)nt * MT + mt) * 128;
            MK4x8(0, 0, vget_low_s16); MK4x8(0, 8, vget_low_s16); MK4x8(4, 0, vget_high_s16); MK4x8(4, 8, vget_high_s16);
        }
    }
}
static void gemm_neon_packed(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    gemm_neon_packed_range(M, N, K, Ap, Bp, Cp, 0, (N + 15) / 16);
}
#endif
#ifdef GEMM_POP_OPTIONS
#pragma GCC pop_options
#endif
#endif
