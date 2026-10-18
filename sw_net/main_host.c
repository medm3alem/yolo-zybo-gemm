/* Version PC : le produit matriciel est calcule en logiciel (reference). Verification contre numpy. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "yolo_core.h"
#include "gemm_pack.h"

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000; }
static void nop(const void *p, size_t n) { (void)p; (void)n; }
static void gemm_sw(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    const int MT = (M + 7) / 8, NT = (N + 15) / 16;
    for (int nt = 0; nt < NT; nt++) for (int mt = 0; mt < MT; mt++) {
        int32_t acc[8][16] = {{0}};
        const int8_t *a = Ap + (size_t)mt * K * 8, *b = Bp + (size_t)nt * K * 16;
        for (int k = 0; k < K; k++) for (int i = 0; i < 8; i++) { int32_t av = a[k * 8 + i]; for (int j = 0; j < 16; j++) acc[i][j] += av * b[k * 16 + j]; }
        int32_t *c = Cp + ((size_t)nt * MT + mt) * 128; for (int i = 0; i < 8; i++) for (int j = 0; j < 16; j++) c[i * 16 + j] = acc[i][j];
    }
}
static void *rd(const char *f, size_t *n) { FILE *fp = fopen(f, "rb"); if (!fp) { perror(f); exit(1); } fseek(fp, 0, SEEK_END); *n = ftell(fp); rewind(fp);
    void *p = aligned_alloc(64, (*n + 63) / 64 * 64); if (fread(p, 1, *n, fp) != *n) exit(1); fclose(fp); return p; }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "."; char path[512]; size_t n;
    snprintf(path, sizeof path, "%s/model.bin", dir); uint8_t *model = rd(path, &n);
    snprintf(path, sizeof path, "%s/img.bin", dir); float *img = rd(path, &n);
    static yolo_stats_t st; yolo_hw_t hw = { gemm_sw, now_us, nop, nop };
    yolo_ctx_t cx = { model, img, aligned_alloc(64, POOL_FLOATS * 4ull), aligned_alloc(64, BP_BYTES_MAX), aligned_alloc(64, C_WORDS_MAX * 4ull), &hw, &st };
    yolo_forward(&cx);
    snprintf(path, sizeof path, "%s/heads_c.bin", dir); FILE *fo = fopen(path, "wb");
    for (int h = 0; h < NHEADS; h++) { int c, hh, ww; const float *p = yolo_head(&cx, h, &c, &hh, &ww); fwrite(p, 4, (size_t)c * hh * ww, fo); } fclose(fo);
    static yolo_det_t d[100]; int nd = yolo_decode(&cx, 0.25f, 0.45f, d, 100);
    snprintf(path, sizeof path, "%s/det_c.txt", dir); fo = fopen(path, "w");
    for (int i = 0; i < nd; i++) fprintf(fo, "%d %.5f %.4f %.4f %.4f %.4f\n", d[i].cls, d[i].score, d[i].x1, d[i].y1, d[i].x2, d[i].y2); fclose(fo);
    fprintf(stderr, "reseau: %.3f s (conv: quant+pack %.3f, gemm %.3f, post %.3f ; autres %.3f), %d detections\n", st.total / 1e6,
            ({ double s = 0; for (int i = 0; i < NCONV; i++) s += st.conv[i].quant; s / 1e6; }), ({ double s = 0; for (int i = 0; i < NCONV; i++) s += st.conv[i].gemm; s / 1e6; }),
            ({ double s = 0; for (int i = 0; i < NCONV; i++) s += st.conv[i].post; s / 1e6; }), st.other_total / 1e6, nd);
    return 0;
}
