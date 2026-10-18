/*
 * main_zybo.c - YOLOv4-tiny int8 complet sur Zybo Z7-20 : ARM (im2col, quantification, post-traitement) + IP gemm_tile (produits matriciels)
 * model.bin et img.bin sont charges en DDR par JTAG (run_yolo.tcl) aux adresses ci-dessous.
 */
#include <stdio.h>
#include <string.h>
#include "xparameters.h"
#include "xgemm_tile.h"
#include "xtime_l.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "yolo_core.h"
#include "expected.h"

#define MODEL_ADDR 0x20000000u
#define IMG_ADDR   0x21000000u

static XGemm_tile ip;
static float   pool[POOL_FLOATS]            __attribute__((aligned(64)));
static int8_t  bpbuf[BP_BYTES_MAX]          __attribute__((aligned(64)));
static int8_t  qxbuf[QX_BYTES]              __attribute__((aligned(64)));
static int32_t cbuf[C_WORDS_MAX]            __attribute__((aligned(64)));
static yolo_stats_t stats;
static yolo_det_t det[100];

static const char *COCO[80] = {"person","bicycle","car","motorbike","aeroplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","sofa","pottedplant","bed","diningtable","toilet","tvmonitor","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};

static uint64_t now_us(void) { XTime t; XTime_GetTime(&t); return (uint64_t)((t * 1000000ULL) / COUNTS_PER_SECOND); }
static void do_flush(const void *p, size_t n) { Xil_DCacheFlushRange((UINTPTR)p, n); }
static void do_inval(const void *p, size_t n) { Xil_DCacheInvalidateRange((UINTPTR)p, n); }

static int g_timeout = 0;
static void gemm_ip(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp)
{
    XGemm_tile_Set_Ap_r(&ip, (u64)(UINTPTR)Ap);
    XGemm_tile_Set_Bp(&ip, (u64)(UINTPTR)Bp);
    XGemm_tile_Set_C(&ip, (u64)(UINTPTR)Cp);
    XGemm_tile_Set_M(&ip, M); XGemm_tile_Set_N(&ip, N); XGemm_tile_Set_K(&ip, K);
    XTime t0, t1; XTime_GetTime(&t0);
    XGemm_tile_Start(&ip);
    while (!XGemm_tile_IsDone(&ip)) { XTime_GetTime(&t1); if ((t1 - t0) > (XTime)20 * COUNTS_PER_SECOND) { g_timeout = 1; return; } }
}

int main(void)
{
    xil_printf("\r\n=== YOLOv4-tiny int8 : ARM + IP gemm_tile ===\r\n");
    XGemm_tile_Config *cfg = XGemm_tile_LookupConfig(XPAR_GEMM_TILE_0_DEVICE_ID);
    if (!cfg || XGemm_tile_CfgInitialize(&ip, cfg) != XST_SUCCESS) { xil_printf("Erreur : init IP\r\n"); return -1; }
    const uint8_t *model = (const uint8_t *)MODEL_ADDR; const float *img = (const float *)IMG_ADDR;
    if (memcmp(model, "YOLM", 4) != 0) { xil_printf("Erreur : model.bin absent a 0x%08x (chargement JTAG ?)\r\n", MODEL_ADDR); return -2; }

    static const yolo_hw_t hw = { gemm_ip, now_us, do_flush, do_inval };
    yolo_ctx_t cx = { model, img, pool, bpbuf, qxbuf, cbuf, &hw, &stats };
    xil_printf("Inference...\r\n");
    yolo_forward(&cx);
    if (g_timeout) { xil_printf("TIMEOUT de l'IP\r\n"); return -3; }

    /* empreinte des sorties : doit egaler celle de la reference numpy */
    uint64_t h = 1469598103934665603ULL; unsigned nw = 0;
    for (int k = 0; k < NHEADS; k++) {
        int c, hh, ww; const float *p = yolo_head(&cx, k, &c, &hh, &ww); const uint32_t *w = (const uint32_t *)p; size_t n = (size_t)c * hh * ww;
        for (size_t i = 0; i < n; i++) { h ^= w[i]; h *= 1099511628211ULL; } nw += n;
    }
    unsigned hi = (unsigned)(h >> 32), lo = (unsigned)h;
    xil_printf("HASH,%08x%08x,attendu,%08x%08x,mots,%u,%s\r\n", hi, lo, EXPECTED_HASH_HI, EXPECTED_HASH_LO, nw,
               (hi == EXPECTED_HASH_HI && lo == EXPECTED_HASH_LO && nw == HEADS_WORDS) ? "IDENTIQUE" : "DIFFERENT");

    uint64_t sq = 0, sf = 0, sg = 0, sp = 0;
    for (int i = 0; i < NCONV; i++) {
        const yolo_t_conv *c = &stats.conv[i];
        xil_printf("LAYER,%d,quant_us,%u,flush_us,%u,ip_us,%u,post_us,%u\r\n", i, (unsigned)c->quant, (unsigned)c->flush, (unsigned)c->gemm, (unsigned)c->post);
        sq += c->quant; sf += c->flush; sg += c->gemm; sp += c->post;
    }
    xil_printf("TOTAL,quant_us,%u,flush_us,%u,ip_us,%u,post_us,%u,autres_us,%u,reseau_us,%u\r\n",
               (unsigned)sq, (unsigned)sf, (unsigned)sg, (unsigned)sp, (unsigned)stats.other_total, (unsigned)stats.total);

    int nd = yolo_decode(&cx, 0.25f, 0.45f, det, 100);
    xil_printf("DETECTIONS,%d\r\n", nd);
    for (int i = 0; i < nd; i++)
        xil_printf("DET,%d,%s,score_milli,%d,x1,%d,y1,%d,x2,%d,y2,%d\r\n", det[i].cls, COCO[det[i].cls], (int)(det[i].score * 1000.f),
                   (int)(det[i].x1 * 416.f), (int)(det[i].y1 * 416.f), (int)(det[i].x2 * 416.f), (int)(det[i].y2 * 416.f));
    xil_printf("=== Fin ===\r\n");
    return 0;
}
