/* yolo_core.h - YOLOv4-tiny int8 portable (PC et Zybo). Le produit matriciel est fourni par la plateforme. */
#ifndef YOLO_CORE_H
#define YOLO_CORE_H
#include <stdint.h>
#include <stddef.h>
#include "net_def.h"

#define QX_BYTES 1441792
typedef struct {
    /* C = A x B pour A (M x K, deja au format IP), B (au format IP) ; C au format tuiles de l'IP */
    void     (*gemm)(int M, int N, int K, const int8_t *Ap, const int8_t *Bp, int32_t *Cp);
    uint64_t (*now_us)(void);
    void     (*flush)(const void *p, size_t n);      /* ecriture ARM -> memoire (avant l'IP) */
    void     (*inval)(const void *p, size_t n);      /* memoire -> ARM (apres l'IP) */
} yolo_hw_t;

typedef struct { uint64_t quant, flush, gemm, post, other; } yolo_t_conv;
typedef struct {
    yolo_t_conv conv[NCONV];
    uint64_t other_total, total;
} yolo_stats_t;

typedef struct { float score; int cls; float x1, y1, x2, y2; } yolo_det_t;

typedef struct {
    const uint8_t *model;      /* model.bin */
    const float   *image;      /* 3 x 416 x 416 float32 CHW, 0..1 */
    float         *pool;       /* POOL_FLOATS floats */
    int8_t        *bp;         /* BP_BYTES_MAX octets, alignes 32 */
    int8_t        *qx;         /* QX_BYTES octets : tenseur d'entree quantifie */
    int32_t       *cbuf;       /* C_WORDS_MAX mots, alignes 32 */
    const yolo_hw_t *hw;
    yolo_stats_t  *stats;
} yolo_ctx_t;

int yolo_forward(const yolo_ctx_t *cx);                 /* 0 = ok */
const float *yolo_head(const yolo_ctx_t *cx, int h, int *c, int *hh, int *ww);
int yolo_decode(const yolo_ctx_t *cx, float thr, float nms_iou, yolo_det_t *out, int max_out);
#endif
