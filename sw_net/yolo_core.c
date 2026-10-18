#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "yolo_core.h"
#include "gemm_pack.h"

typedef struct { uint32_t M, K, a_off, s_off, b_off, r0, r1, r2; } conv_hdr_t;

static inline float rne(float v) { return (v + 12582912.0f) - 12582912.0f; }   /* arrondi pair, |v| < 2^22 */

/* quantification int8 du tenseur d'entree, UNE seule fois, dans un plan a bordure de zeros (pd) : plus aucun test de bord ensuite */
static void quant_tensor_pad(const float *x, int C, int H, int W, int pd, float sB, int8_t *q)
{
    const int Hp = H + 2 * pd, Wp = W + 2 * pd;
    if (pd) memset(q, 0, (size_t)C * Hp * Wp);
    for (int c = 0; c < C; c++)
        for (int y = 0; y < H; y++) {
            const float *r = x + ((size_t)c * H + y) * W; int8_t *o = q + ((size_t)c * Hp + y + pd) * Wp + pd;
            for (int i = 0; i < W; i++) {
                float v = rne(r[i] * sB);
                if (v > 127.0f) v = 127.0f; else if (v < -127.0f) v = -127.0f;
                o[i] = (int8_t)(int)v;
            }
        }
}

/* im2col sur le plan a bordure, ecrit directement au format panneau B de l'IP : Bp[nt][k][16] */
static void im2col_pack(const int8_t *q, int C, int H, int W, int ks, int st, int Ho, int Wo, int8_t *Bp)
{
    const int pd = ks / 2, Hp = H + 2 * pd, Wp = W + 2 * pd, N = Ho * Wo, K = C * ks * ks, NT = (N + 15) / 16;
    const int HWp = Hp * Wp;
    for (int nt = 0; nt < NT; nt++) {
        const int n0 = nt * 16, jmax = (N - n0 < 16) ? N - n0 : 16;
        int off[16], oy0 = n0 / Wo, oy15 = 0;
        for (int j = 0; j < 16; j++) {
            int n = n0 + (j < jmax ? j : 0), oy = n / Wo, ox = n - oy * Wo;
            off[j] = oy * st * Wp + ox * st; if (j == 15) oy15 = oy;
        }
        const int contig = (st == 1) && (jmax == 16) && (oy0 == oy15);
        int8_t *dst = Bp + (size_t)nt * K * 16;
        for (int c = 0; c < C; c++) {
            const int8_t *pl = q + (size_t)c * HWp;
            for (int ky = 0; ky < ks; ky++)
                for (int kx = 0; kx < ks; kx++, dst += 16) {
                    const int8_t *p = pl + ky * Wp + kx;
                    if (contig) { memcpy(dst, p + off[0], 16); continue; }
                    for (int j = 0; j < 16; j++) dst[j] = p[off[j]];
                }
        }
    }
}

static void conv_op(const yolo_ctx_t *cx, const op_t *op, const float *in, float *out, int ci_idx)
{
    const yolo_hw_t *hw = cx->hw; yolo_t_conv *st = &cx->stats->conv[ci_idx];
    const conv_hdr_t *h = (const conv_hdr_t *)(cx->model + 64) + op->conv;
    const int M = (int)h->M, K = (int)h->K, N = op->ho * op->wo;
    const int8_t *Ap = (const int8_t *)(cx->model + h->a_off);
    const float *sinv = (const float *)(cx->model + h->s_off), *bias = (const float *)(cx->model + h->b_off);
    uint64_t t0 = hw->now_us();

    /* echelle d'activation dynamique */
    const size_t nin = (size_t)op->ci * op->hi * op->wi; float mx = 0.f;
    for (size_t i = 0; i < nin; i++) { float a = in[i] < 0 ? -in[i] : in[i]; if (a > mx) mx = a; }
    float sB = mx > 0.f ? 127.0f / mx : 1.0f, invsB = mx > 0.f ? mx / 127.0f : 1.0f;
    { int pd = op->k / 2; if ((size_t)op->ci * (op->hi + 2 * pd) * (op->wi + 2 * pd) > QX_BYTES) return; }
    quant_tensor_pad(in, op->ci, op->hi, op->wi, op->k / 2, sB, cx->qx);
    im2col_pack(cx->qx, op->ci, op->hi, op->wi, op->k, op->s, op->ho, op->wo, cx->bp);
    uint64_t t1 = hw->now_us();
    hw->flush(cx->bp, gp_bytesB(K, N));
    uint64_t t2 = hw->now_us();
    hw->gemm(M, N, K, Ap, cx->bp, cx->cbuf);
    uint64_t t3 = hw->now_us();
    hw->inval(cx->cbuf, gp_bytesC(M, N));

    const int MT = (M + 7) / 8, NT = (N + 15) / 16;
    for (int nt = 0; nt < NT; nt++) {
        const int jmax = (N - nt * 16 < 16) ? N - nt * 16 : 16;
        for (int m = 0; m < M; m++) {
            const int32_t *c = cx->cbuf + ((size_t)nt * MT + (m >> 3)) * 128 + (m & 7) * 16;
            const float sc = sinv[m] * invsB, b = bias[m];
            float *o = out + (size_t)m * N + nt * 16;
            if (op->leaky) for (int j = 0; j < jmax; j++) { float v = (float)c[j] * sc + b; o[j] = v > 0.f ? v : 0.1f * v; }
            else           for (int j = 0; j < jmax; j++) o[j] = (float)c[j] * sc + b;
        }
    }
    uint64_t t4 = hw->now_us();
    st->quant = t1 - t0; st->flush = t2 - t1; st->gemm = t3 - t2; st->post = t4 - t3;
}

int yolo_forward(const yolo_ctx_t *cx)
{
    const yolo_hw_t *hw = cx->hw; uint64_t T0 = hw->now_us(); uint64_t other = 0;
    for (int i = 0; i < NOPS; i++) {
        const op_t *op = &OPS[i]; uint64_t t0 = hw->now_us();
        float *out = cx->pool + op->out_off;
        const float *in = op->in_op < 0 ? cx->image : cx->pool + OPS[op->in_op].out_off;
        switch (op->type) {
        case 0: conv_op(cx, op, in, out, op->conv); continue;                 /* temps compte par couche */
        case 1: {                                                              /* route */
            if (op->groups > 1) {
                const op_t *s = &OPS[op->s0]; size_t cn = (size_t)(s->co / op->groups) * s->ho * s->wo;
                memcpy(out, cx->pool + s->out_off + cn * op->gid, cn * sizeof(float));
            } else {
                size_t o = 0;
                for (int q = 0; q < op->nsrc; q++) {
                    const op_t *s = &OPS[q ? op->s1 : op->s0]; size_t n = (size_t)s->co * s->ho * s->wo;
                    memcpy(out + o, cx->pool + s->out_off, n * sizeof(float)); o += n;
                }
            }
            break; }
        case 2: {                                                              /* maxpool 2x2 /2 */
            for (int c = 0; c < op->co; c++)
                for (int y = 0; y < op->ho; y++) {
                    const float *r0 = in + ((size_t)c * op->hi + 2 * y) * op->wi, *r1 = r0 + op->wi; float *o = out + ((size_t)c * op->ho + y) * op->wo;
                    for (int x = 0; x < op->wo; x++) {
                        float a = r0[2 * x] > r0[2 * x + 1] ? r0[2 * x] : r0[2 * x + 1], b = r1[2 * x] > r1[2 * x + 1] ? r1[2 * x] : r1[2 * x + 1];
                        o[x] = a > b ? a : b;
                    }
                }
            break; }
        case 3: {                                                              /* upsample x2 */
            for (int c = 0; c < op->co; c++)
                for (int y = 0; y < op->ho; y++) {
                    const float *r = in + ((size_t)c * op->hi + y / 2) * op->wi; float *o = out + ((size_t)c * op->ho + y) * op->wo;
                    for (int x = 0; x < op->wo; x++) o[x] = r[x / 2];
                }
            break; }
        default: break;                                                        /* yolo : alias de l'entree */
        }
        other += hw->now_us() - t0;
    }
    cx->stats->other_total = other; cx->stats->total = hw->now_us() - T0;
    return 0;
}

const float *yolo_head(const yolo_ctx_t *cx, int h, int *c, int *hh, int *ww)
{
    for (int i = 0; i < NOPS; i++) if (OPS[i].type == 4 && OPS[i].head == h) { *c = OPS[i].co; *hh = OPS[i].ho; *ww = OPS[i].wo; return cx->pool + OPS[i].out_off; }
    return 0;
}

static inline float sigm(float z) { return 1.0f / (1.0f + expf(-z)); }
static int cmp_det(const void *a, const void *b) { float x = ((const yolo_det_t *)a)->score, y = ((const yolo_det_t *)b)->score; return (x < y) - (x > y); }
static float iou_d(const yolo_det_t *a, const yolo_det_t *b)
{
    float ix = fminf(a->x2, b->x2) - fmaxf(a->x1, b->x1), iy = fminf(a->y2, b->y2) - fmaxf(a->y1, b->y1);
    if (ix < 0) ix = 0; if (iy < 0) iy = 0; float i = ix * iy, u = (a->x2 - a->x1) * (a->y2 - a->y1) + (b->x2 - b->x1) * (b->y2 - b->y1) - i;
    return u > 0 ? i / u : 0;
}

#define MAXCAND 6000
int yolo_decode(const yolo_ctx_t *cx, float thr, float nms_iou, yolo_det_t *out, int max_out)
{
    static yolo_det_t cand[MAXCAND]; int nc_ = 0;
    for (int h = 0; h < NHEADS; h++) {
        int C, H, W; const float *o = yolo_head(cx, h, &C, &H, &W); const yolo_cfg_t *y = &YOLO[h]; const int ncl = y->nc;
        for (int a = 0; a < y->nmask; a++) {
            const float aw = y->anchors[2 * y->mask[a]], ah = y->anchors[2 * y->mask[a] + 1];
            const float *tx = o + (size_t)(a * (5 + ncl) + 0) * H * W, *ty = tx + H * W, *tw = ty + H * W, *th = tw + H * W, *to = th + H * W;
            for (int gy = 0; gy < H; gy++) for (int gx = 0; gx < W; gx++) {
                const size_t p = (size_t)gy * W + gx; const float obj = sigm(to[p]);
                if (obj <= thr) continue;                                    /* classe <= 1 : score <= obj */
                const float sxy = y->scale_x_y;
                float bx = (sigm(tx[p]) * sxy - 0.5f * (sxy - 1.f) + gx) / W, by = (sigm(ty[p]) * sxy - 0.5f * (sxy - 1.f) + gy) / H;
                float bw = expf(tw[p]) * aw / NET_W, bh = expf(th[p]) * ah / NET_W;
                for (int c = 0; c < ncl; c++) {
                    float sc = obj * sigm(o[(size_t)(a * (5 + ncl) + 5 + c) * H * W + p]);
                    if (sc > thr && nc_ < MAXCAND) { yolo_det_t d = { sc, c, bx - bw / 2, by - bh / 2, bx + bw / 2, by + bh / 2 }; cand[nc_++] = d; }
                }
            }
        }
    }
    qsort(cand, nc_, sizeof(yolo_det_t), cmp_det);
    if (nc_ > 300) nc_ = 300;
    int n = 0;
    for (int i = 0; i < nc_; i++) {
        int keep = 1;
        for (int j = 0; j < n; j++) if (out[j].cls == cand[i].cls && iou_d(&out[j], &cand[i]) > nms_iou) { keep = 0; break; }
        if (keep && n < max_out) out[n++] = cand[i];
    }
    return n;
}
