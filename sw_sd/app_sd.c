/*
 * app_sd.c - traitement par lots : images de la carte SD -> YOLOv4-tiny int8 -> detections + temps, ecrits sur la carte.
 * Code commun Zybo / PC. Prétraitement (decodage JPEG/PNG/BMP, redimensionnement type PIL, letterbox) sur l'ARM.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include "app_sd.h"
#include "fsio.h"
#include "font8x12.h"
#include "gemm_arm.h"

/* ---------- memoire statique ---------- */
#define ARENA_BYTES (96u << 20)
#define MODEL_MAX   (7u << 20)
static uint8_t arena[ARENA_BYTES] __attribute__((aligned(64)));
static size_t  arena_top;
static uint8_t model_buf[MODEL_MAX] __attribute__((aligned(64)));
static float   canvas[3 * NET_W * NET_W] __attribute__((aligned(64)));
static float   pool[POOL_FLOATS] __attribute__((aligned(64)));
static int8_t  bpbuf[BP_BYTES_MAX] __attribute__((aligned(64)));
static int8_t  qxbuf[QX_BYTES] __attribute__((aligned(64)));
static int32_t cbuf[C_WORDS_MAX] __attribute__((aligned(64)));
static yolo_stats_t stats;
static yolo_det_t det[100];
static char report[64 * 1024];

/* ---------- allocateur a pile pour stb_image et nos tampons ---------- */
void *arena_alloc(size_t n)
{
    size_t need = (n + 63) & ~(size_t)63;
    if (arena_top + 64 + need > ARENA_BYTES) return NULL;
    uint8_t *p = arena + arena_top; *(size_t *)p = n; arena_top += 64 + need;
    return p + 64;
}
void *arena_realloc(void *old, size_t n)
{
    if (!old) return arena_alloc(n);
    uint8_t *h = (uint8_t *)old - 64; size_t on = *(size_t *)h;
    if (h + 64 + ((on + 63) & ~(size_t)63) == arena + arena_top) {              /* dernier bloc : on l'etend sur place */
        size_t need = (n + 63) & ~(size_t)63;
        if ((size_t)(h - arena) + 64 + need > ARENA_BYTES) return NULL;
        arena_top = (size_t)(h - arena) + 64 + need; *(size_t *)h = n; return old;
    }
    void *q = arena_alloc(n); if (q) memcpy(q, old, on < n ? on : n); return q;
}
static void arena_free(void *p) { (void)p; }

#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS   /* pas de variables __thread en bare-metal : le drapeau "retourner a la lecture" serait aleatoire */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_MALLOC(sz)        arena_alloc(sz)
#define STBI_REALLOC(p, sz)    arena_realloc(p, sz)
#define STBI_FREE(p)           arena_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---------- classes COCO ---------- */
static const char *COCO[80] = {"person","bicycle","car","motorbike","aeroplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","sofa","pottedplant","bed","diningtable","toilet","tvmonitor","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};

/* ---------- redimensionnement (filtre triangle a support elargi, comme PIL BILINEAR) ---------- */
static int make_coeffs(int in, int out, float **kk_, int **xmin_, int **xn_, int *ksize_)
{
    float scale = (float)in / out, fs = scale < 1.f ? 1.f : scale, support = fs;
    int ks = (int)ceilf(support) * 2 + 1;
    float *kk = arena_alloc((size_t)out * ks * sizeof(float)); int *xmin = arena_alloc((size_t)out * 2 * sizeof(int));
    if (!kk || !xmin) return -1;
    for (int xx = 0; xx < out; xx++) {
        float center = (xx + 0.5f) * scale, ww = 0.f; int lo = (int)(center - support + 0.5f), hi = (int)(center + support + 0.5f);
        if (lo < 0) lo = 0; if (hi > in) hi = in; hi -= lo;
        for (int x = 0; x < hi; x++) { float d = fabsf((x + lo - center + 0.5f) / fs); float w = d < 1.f ? 1.f - d : 0.f; kk[xx * ks + x] = w; ww += w; }
        for (int x = 0; x < hi; x++) kk[xx * ks + x] = ww != 0.f ? kk[xx * ks + x] / ww : 0.f;
        for (int x = hi; x < ks; x++) kk[xx * ks + x] = 0.f;
        xmin[2 * xx] = lo; xmin[2 * xx + 1] = hi;
    }
    *kk_ = kk; *xmin_ = xmin; *xn_ = xmin; *ksize_ = ks; return 0;
}
static inline uint8_t clip8(float v) { int i = (int)(v + 0.5f); return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i); }

/* rgb (w x h x 3 entrelaces) -> canvas float CHW 416x416 avec bandes grises. Renvoie 0 si ok. */
static int letterbox(const uint8_t *rgb, int w, int h, int *nw_, int *nh_, int *ox_, int *oy_)
{
    float s = fminf((float)NET_W / w, (float)NET_W / h);
    int nw = (int)floorf(w * s + 0.5f), nh = (int)floorf(h * s + 0.5f); if (nw < 1) nw = 1; if (nh < 1) nh = 1;
    int ox = (NET_W - nw) / 2, oy = (NET_W - nh) / 2;
    size_t mark = arena_top;
    float *kx, *ky; int *mx, *nx, *my, *ny, ksx, ksy;
    if (make_coeffs(w, nw, &kx, &mx, &nx, &ksx) || make_coeffs(h, nh, &ky, &my, &ny, &ksy)) { arena_top = mark; return -1; }
    uint8_t *tmp = arena_alloc((size_t)nw * h * 3), *res = arena_alloc((size_t)nw * nh * 3);
    if (!tmp || !res) { arena_top = mark; return -1; }
    for (int y = 0; y < h; y++) {                                              /* passe horizontale, arrondie en 8 bits */
        const uint8_t *r = rgb + (size_t)y * w * 3; uint8_t *o = tmp + (size_t)y * nw * 3;
        for (int x = 0; x < nw; x++) {
            const float *k = kx + x * ksx; const uint8_t *p = r + (size_t)mx[2 * x] * 3; int n = mx[2 * x + 1]; float a = 0, b = 0, c = 0;
            for (int t = 0; t < n; t++, p += 3) { a += p[0] * k[t]; b += p[1] * k[t]; c += p[2] * k[t]; }
            o[3 * x] = clip8(a); o[3 * x + 1] = clip8(b); o[3 * x + 2] = clip8(c);
        }
    }
    for (int y = 0; y < nh; y++) {                                             /* passe verticale */
        const float *k = ky + y * ksy; int y0 = my[2 * y], n = my[2 * y + 1]; uint8_t *o = res + (size_t)y * nw * 3;
        for (int x = 0; x < nw * 3; x++) {
            float a = 0; const uint8_t *p = tmp + (size_t)y0 * nw * 3 + x;
            for (int t = 0; t < n; t++, p += (size_t)nw * 3) a += *p * k[t];
            o[x] = clip8(a);
        }
    }
    const float g = 128.0f / 255.0f;
    for (size_t i = 0; i < 3u * NET_W * NET_W; i++) canvas[i] = g;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < 3; c++) canvas[((size_t)c * NET_W + oy + y) * NET_W + ox + x] = res[((size_t)y * nw + x) * 3 + c] / 255.0f;
    arena_top = mark; *nw_ = nw; *nh_ = nh; *ox_ = ox; *oy_ = oy; return 0;
}

/* ---------- dessin sur l'image d'origine + ecriture BMP ---------- */
static const uint8_t PAL[8][3] = {{255,40,40},{40,220,40},{70,110,255},{255,220,0},{255,0,255},{0,230,230},{255,140,0},{160,80,255}};
static void put_px(uint8_t *rgb, int w, int h, int x, int y, const uint8_t *c) { if (x >= 0 && y >= 0 && x < w && y < h) { uint8_t *p = rgb + ((size_t)y * w + x) * 3; p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; } }
static void fill_rect(uint8_t *rgb, int w, int h, int x0, int y0, int x1, int y1, const uint8_t *c) { for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) put_px(rgb, w, h, x, y, c); }
static void draw_text(uint8_t *rgb, int w, int h, int x0, int y0, const char *s, int sc, const uint8_t *fg)
{
    for (; *s; s++, x0 += 8 * sc) {
        int ch = (unsigned char)*s; if (ch < 32 || ch > 126) ch = '?';
        for (int y = 0; y < 12; y++) for (int x = 0; x < 8; x++)
            if (FONT8X12[ch - 32][y] >> (7 - x) & 1) fill_rect(rgb, w, h, x0 + x * sc, y0 + y * sc, x0 + x * sc + sc - 1, y0 + y * sc + sc - 1, fg);
    }
}
static int write_bmp(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t rs = ((size_t)w * 3 + 3) & ~(size_t)3, sz = 54 + rs * h; size_t mark = arena_top; uint8_t *b = arena_alloc(sz);
    if (!b) { arena_top = mark; return -1; }
    memset(b, 0, 54); b[0] = 'B'; b[1] = 'M'; b[2] = sz; b[3] = sz >> 8; b[4] = sz >> 16; b[5] = sz >> 24; b[10] = 54; b[14] = 40;
    b[18] = w; b[19] = w >> 8; b[20] = w >> 16; b[21] = w >> 24; b[22] = h; b[23] = h >> 8; b[24] = h >> 16; b[25] = h >> 24; b[26] = 1; b[28] = 24;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgb + (size_t)(h - 1 - y) * w * 3; uint8_t *d = b + 54 + rs * y;
        for (int x = 0; x < w; x++) { d[3 * x] = s[3 * x + 2]; d[3 * x + 1] = s[3 * x + 1]; d[3 * x + 2] = s[3 * x]; }
        for (size_t x = (size_t)w * 3; x < rs; x++) d[x] = 0;
    }
    int r = fs_write(path, b, sz); arena_top = mark; return r;
}

/* ---------- orientation EXIF (photos de telephone) : stb_image ne la lit pas ---------- */
static int exif_orientation(const uint8_t *d, size_t n)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    size_t p = 2;
    while (p + 4 <= n && d[p] == 0xFF) {
        int m = d[p + 1]; size_t len = ((size_t)d[p + 2] << 8) | d[p + 3];
        if (m == 0xDA || m == 0xD9) break;
        if (m == 0xE1 && len >= 14 && p + 2 + len <= n && !memcmp(d + p + 4, "Exif\0\0", 6)) {
            const uint8_t *t = d + p + 10; size_t tn = len - 8; int le = (t[0] == 'I');
            #define R16(o) (le ? (t[o] | (t[(o) + 1] << 8)) : ((t[o] << 8) | t[(o) + 1]))
            #define R32(o) (le ? ((uint32_t)t[o] | ((uint32_t)t[(o)+1] << 8) | ((uint32_t)t[(o)+2] << 16) | ((uint32_t)t[(o)+3] << 24)) : (((uint32_t)t[o] << 24) | ((uint32_t)t[(o)+1] << 16) | ((uint32_t)t[(o)+2] << 8) | t[(o)+3]))
            if (tn < 8) return 1;
            size_t io = R32(4); if (io + 2 > tn) return 1;
            int ne = R16(io);
            for (int i = 0; i < ne && io + 2 + (size_t)(i + 1) * 12 <= tn; i++) {
                size_t e = io + 2 + (size_t)i * 12;
                if (R16(e) == 0x0112) { int v = R16(e + 8); return (v >= 1 && v <= 8) ? v : 1; }
            }
            return 1;
        }
        p += 2 + len;
    }
    return 1;
}
/* remet l'image a l'endroit (copie dans l'arene) ; renvoie le nouveau tampon ou NULL */
static uint8_t *apply_orientation(const uint8_t *s, int *w_, int *h_, int o)
{
    int w = *w_, h = *h_, ow = (o >= 5) ? h : w, oh = (o >= 5) ? w : h;
    uint8_t *d = arena_alloc((size_t)ow * oh * 3); if (!d) return NULL;
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        int sx, sy;
        switch (o) { case 2: sx = w - 1 - x; sy = y; break; case 3: sx = w - 1 - x; sy = h - 1 - y; break; case 4: sx = x; sy = h - 1 - y; break;
                     case 5: sx = y; sy = x; break; case 6: sx = y; sy = h - 1 - x; break; case 7: sx = w - 1 - y; sy = h - 1 - x; break; default: sx = w - 1 - y; sy = x; break; }
        const uint8_t *p = s + ((size_t)sy * w + sx) * 3; uint8_t *q = d + ((size_t)y * ow + x) * 3; q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
    }
    *w_ = ow; *h_ = oh; return d;
}

/* ---------- empreintes de diagnostic (comparees aux valeurs obtenues sur PC) ---------- */
static uint64_t fnv64(const void *p, size_t n)
{
    const uint8_t *b = p; uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
#define H64(x) (unsigned)((x) >> 32), (unsigned)(x)

/* ---------- utilitaires ---------- */
static int app_cat(char *o, size_t cap, size_t *len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static int app_cat(char *o, size_t cap, size_t *len, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vsnprintf(o + *len, cap - *len, fmt, ap); va_end(ap); if (n > 0 && *len + n < cap) *len += n; return n; }
static void base_name(const char *name, char *out) { strncpy(out, name, 63); out[63] = 0; char *d = strrchr(out, '.'); if (d) *d = 0; }
#define MS(us) (unsigned)((us) / 1000), (unsigned)((us) % 1000)

/* ---------- configuration optionnelle : config.txt a la racine de la carte ----------
 *   modes=ip,neon,neon2,scalar (sous-ensemble ; defaut : ip)   ip = FPGA ; neon = ARM seul (NEON, 1 coeur) ; neon2 = ARM seul (NEON, 2 coeurs,
 *                              n'existe que si le programme du coeur 1 est charge par JTAG) ; scalar = ARM seul (C simple, lent)
 *   repeat=3                 (repetitions par image et par mode ; defaut 1 ; le mode scalar n'est execute qu'une fois)
 *   diag=0                   (1 = affiche des empreintes de diagnostic)
 *   production=1             (mode production : mode ip, 1 passage, et SEULE l'image annotee <nom>_det.bmp est ecrite dans out/ ;
 *                             ni .txt, ni resultats.csv, ni benchmark.csv, ni resume.txt)
 */
#define MODE_IP 0
#define MODE_NEON 1
#define MODE_SCALAR 2
#define MODE_NEON2 3
#define NMODES 4
static const char *MODE_NAME[NMODES] = { "ip", "neon", "scalar", "neon2" };
typedef struct { int modes[NMODES]; int nmodes; int repeat; int diag; int production; } app_cfg_t;
static void parse_cfg(const char *root, app_cfg_t *c)
{
    char path[160], buf[2048]; size_t n = 0;
    c->modes[0] = MODE_IP; c->nmodes = 1; c->repeat = 1; c->diag = 0; c->production = 0;
    snprintf(path, sizeof path, "%sconfig.txt", root);
    if (fs_read(path, buf, sizeof buf - 1, &n) || n == 0) return;
    buf[n] = 0;
    for (char *l = buf; *l; ) {                                                /* decoupe en lignes a la main (pas de strtok imbrique) */
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        char *eq = strchr(l, '=');
        if (eq) {
            *eq = 0; const char *k = l, *v = eq + 1;
            if (!strcmp(k, "repeat")) { int r = atoi(v); c->repeat = r < 1 ? 1 : r > 50 ? 50 : r; }
            else if (!strcmp(k, "diag")) c->diag = atoi(v) != 0;
            else if (!strcmp(k, "production")) c->production = atoi(v) != 0;
            else if (!strcmp(k, "modes")) {
                c->nmodes = 0;
                for (const char *t = v; *t && c->nmodes < NMODES; ) {
                    while (*t == ',' || *t == ' ') t++;
                    const char *te = t; while (*te && *te != ',' && *te != ' ') te++;
                    for (int m = 0; m < NMODES; m++) if (strlen(MODE_NAME[m]) == (size_t)(te - t) && !strncmp(t, MODE_NAME[m], te - t)) c->modes[c->nmodes++] = m;
                    t = te;
                }
                if (!c->nmodes) { c->modes[0] = MODE_IP; c->nmodes = 1; }
            }
        }
        if (!sv) break;
        l = e + 1; while (*l == '\n' || *l == '\r') l++;
    }
    if (c->production) { c->modes[0] = MODE_IP; c->nmodes = 1; c->repeat = 1; c->diag = 0; }   /* production : FPGA, un seul calcul par image */
}

static uint64_t heads_hash(const yolo_ctx_t *cx)
{
    uint64_t h = 1469598103934665603ULL;
    for (int k = 0; k < NHEADS; k++) {
        int c, y, x; const float *p = yolo_head(cx, k, &c, &y, &x); const uint32_t *u = (const uint32_t *)p;
        for (size_t i = 0, n = (size_t)c * y * x; i < n; i++) { h ^= u[i]; h *= 1099511628211ULL; }
    }
    return h;
}

typedef struct { double sum_mean_ms; int n; } mode_acc_t;

int app_run(const yolo_hw_t *hw_ip, const yolo_hw_t *hw_neon, const yolo_hw_t *hw_scalar, const yolo_hw_t *hw_neon2, const char *root)
{
    const yolo_hw_t *HW[NMODES] = { hw_ip, hw_neon, hw_scalar, hw_neon2 };
    const yolo_hw_t *hw = hw_ip;                                               /* horloge, E/S : toujours celles de la plateforme */
    char path[160], base[64]; static char names[64][64]; static char bench[32 * 1024]; size_t bl = 0;
    app_cfg_t cfg; parse_cfg(root, &cfg);
    printf("\r\n=== YOLOv4-tiny int8 : traitement des images de la carte SD ===\r\n");
    printf("modes :"); for (int m = 0; m < cfg.nmodes; m++) printf(" %s", MODE_NAME[cfg.modes[m]]); printf(" ; repetitions : %d\r\n", cfg.repeat);
    { int k = 0;                                                                /* modes indisponibles : ignores (message), pas d'arret */
      for (int m = 0; m < cfg.nmodes; m++) {
          if (HW[cfg.modes[m]]) cfg.modes[k++] = cfg.modes[m];
          else printf("Mode %s indisponible dans ce programme : ignore\r\n", MODE_NAME[cfg.modes[m]]);
      }
      cfg.nmodes = k;
      if (!k) { printf("Erreur : aucun mode utilisable\r\n"); return -3; } }

    snprintf(path, sizeof path, "%smodel.bin", root); size_t mn = 0;
    uint64_t t0 = hw->now_us();
    if (fs_read(path, model_buf, MODEL_MAX, &mn) || mn < 64 || memcmp(model_buf, "YOLM", 4)) { printf("Erreur : %s introuvable ou invalide\r\n", path); return -1; }
    hw->flush(model_buf, mn);                                                  /* les poids doivent etre en DDR pour l'IP */
    printf("model.bin : %u octets lus en %u.%03u ms\r\n", (unsigned)mn, MS(hw->now_us() - t0));
    if (cfg.diag) printf("DIAG,model,octets,%u,fnv,%08x%08x\r\n", (unsigned)mn, H64(fnv64(model_buf, mn)));
    snprintf(path, sizeof path, "%simages", root); int ni = fs_list_images(path, names, 64);
    if (ni <= 0) { printf("Erreur : aucune image dans %s\r\n", path); return -2; }
    snprintf(path, sizeof path, "%sout", root); fs_mkdir(path);
    printf("%d image(s)%s\r\n", ni, cfg.production ? " (mode production : images annotees seulement)" : "");

    size_t rl = 0;
    app_cat(report, sizeof report, &rl, "image,largeur,hauteur,lecture_ms,decodage_ms,pretraitement_ms,reseau_ms,decodage_boites_ms,traitement_total_ms,ecriture_ms,detections\n");
    app_cat(bench, sizeof bench, &bl, "image,mode,repetitions,reseau_moy_ms,reseau_min_ms,reseau_max_ms,reseau_ecart_ms,quant_im2col_ms,produits_matriciels_ms,flush_ms,post_ms,autres_ms,sorties_identiques_au_premier_mode\n");
    mode_acc_t acc[NMODES] = {{0,0},{0,0},{0,0},{0,0}};

    for (int im = 0; im < ni; im++) {
        arena_top = 0; char txt[6144]; size_t tl = 0;
        snprintf(path, sizeof path, "%simages/%s", root, names[im]); base_name(names[im], base);
        size_t fsz = 0; uint64_t a0 = hw->now_us();
        uint8_t *fbuf = arena_alloc(ARENA_BYTES / 4);
        if (!fbuf || fs_read(path, fbuf, ARENA_BYTES / 4, &fsz)) { printf("%s : lecture impossible\r\n", names[im]); continue; }
        arena_top = 64 + ((fsz + 63) & ~(size_t)63); *(size_t *)(fbuf - 64) = fsz;     /* on ne garde que la partie utile */
        uint64_t a1 = hw->now_us();
        if (cfg.diag) printf("DIAG,%s,fichier,octets,%u,fnv,%08x%08x\r\n", names[im], (unsigned)fsz, H64(fnv64(fbuf, fsz)));
        int w, h, nc; uint8_t *rgb = stbi_load_from_memory(fbuf, (int)fsz, &w, &h, &nc, 3);
        if (!rgb) { printf("%s : decodage impossible (%s)\r\n", names[im], stbi_failure_reason()); continue; }
        int orient = exif_orientation(fbuf, fsz);
        if (orient > 1) { uint8_t *r2 = apply_orientation(rgb, &w, &h, orient); if (!r2) { printf("%s : memoire insuffisante (rotation)\r\n", names[im]); continue; } rgb = r2; }
        uint64_t a2 = hw->now_us();
        if (cfg.diag) printf("DIAG,%s,decode,%dx%d,orient,%d,fnv,%08x%08x\r\n", names[im], w, h, orient, H64(fnv64(rgb, (size_t)w * h * 3)));
        int nw, nh, ox, oy;
        if (letterbox(rgb, w, h, &nw, &nh, &ox, &oy)) { printf("%s : memoire insuffisante\r\n", names[im]); continue; }
        uint64_t a3 = hw->now_us();
        if (cfg.diag) printf("DIAG,%s,canvas,fnv,%08x%08x\r\n", names[im], H64(fnv64(canvas, sizeof canvas)));

        /* ---- reseau : chaque mode, chaque repetition ---- */
        uint64_t tn = 0, tb = 0, ref_hash = 0, s_q = 0, s_f = 0, s_g = 0, s_p = 0, s_o = 0; int nd = 0;
        for (int mi = 0; mi < cfg.nmodes; mi++) {
            const int mode = cfg.modes[mi]; const yolo_hw_t *mh = HW[mode];
            yolo_ctx_t cx = { model_buf, canvas, pool, bpbuf, qxbuf, cbuf, mh, &stats };
            const int reps = (mode == MODE_SCALAR) ? 1 : cfg.repeat;
            double sum = 0, sum2 = 0; uint64_t mn_us = ~0ULL, mx_us = 0; int same = 1;
            uint64_t q = 0, f = 0, g = 0, p = 0, o = 0;
            for (int r = 0; r < reps; r++) {
                yolo_forward(&cx);
                const uint64_t t = stats.total; sum += (double)t; sum2 += (double)t * (double)t; if (t < mn_us) mn_us = t; if (t > mx_us) mx_us = t;
                uint64_t hh = heads_hash(&cx);
                if (mi == 0 && r == 0) {                                              /* premiere execution : reference + detections */
                    ref_hash = hh; tn = t;
                    uint64_t d0 = hw->now_us(); nd = yolo_decode(&cx, 0.25f, 0.45f, det, 100); tb = hw->now_us() - d0;
                    for (int i = 0; i < NCONV; i++) { s_q += stats.conv[i].quant; s_f += stats.conv[i].flush; s_g += stats.conv[i].gemm; s_p += stats.conv[i].post; }
                    s_o = stats.other_total;
                    if (cfg.diag) printf("DIAG,%s,sorties,fnv,%08x%08x\r\n", names[im], H64(hh));
                }
                if (hh != ref_hash) same = 0;
                if (r == reps - 1) { for (int i = 0; i < NCONV; i++) { q += stats.conv[i].quant; f += stats.conv[i].flush; g += stats.conv[i].gemm; p += stats.conv[i].post; } o = stats.other_total; }
            }
            double mean = sum / reps, var = sum2 / reps - mean * mean; if (var < 0) var = 0; double sd = sqrt(var);
            uint64_t um = (uint64_t)(mean + 0.5), us = (uint64_t)(sd + 0.5);
            app_cat(bench, sizeof bench, &bl, "%s,%s,%d,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%s\n",
                    names[im], MODE_NAME[mode], reps, MS(um), MS(mn_us), MS(mx_us), MS(us), MS(q), MS(g), MS(f), MS(p), MS(o), same ? "oui" : "NON");
            if (!cfg.production) printf("%s [%s] : reseau moy %u.%03u ms (min %u.%03u, max %u.%03u, %d rep.), sorties %s\r\n", names[im], MODE_NAME[mode], MS(um), MS(mn_us), MS(mx_us), reps, same ? "identiques" : "DIFFERENTES");
            acc[mode].sum_mean_ms += mean / 1000.0; acc[mode].n++;
        }

        uint64_t a4 = a3 + tn, a5 = a4 + tb;                                     /* temps cumules de la premiere execution */
        uint64_t tr = a1 - a0, td = a2 - a1, tp = a3 - a2, tt = (a2 - a1) + (a3 - a2) + tn + tb;
        (void)a5;
        app_cat(txt, sizeof txt, &tl, "image : %s (%dx%d, orientation EXIF %d)\n", names[im], w, h, orient);
        app_cat(txt, sizeof txt, &tl, "mode du premier calcul : %s\n", MODE_NAME[cfg.modes[0]]);
        app_cat(txt, sizeof txt, &tl, "decodage image : %u.%03u ms\npretraitement (redim. + letterbox) : %u.%03u ms\nreseau YOLOv4-tiny : %u.%03u ms\n", MS(td), MS(tp), MS(tn));
        app_cat(txt, sizeof txt, &tl, "  dont quantification+im2col (ARM) : %u.%03u ms\n  dont produits matriciels (%s)  : %u.%03u ms\n  dont flush cache                 : %u.%03u ms\n  dont post-traitement (ARM)       : %u.%03u ms\n  dont routes/maxpool/upsample     : %u.%03u ms\n", MS(s_q), cfg.modes[0] == MODE_IP ? "FPGA" : "ARM ", MS(s_g), MS(s_f), MS(s_p), MS(s_o));
        app_cat(txt, sizeof txt, &tl, "decodage des boites + NMS : %u.%03u ms\nTOTAL (decodage image -> detections) : %u.%03u ms\n\ndetections : %d\n", MS(tb), MS(tt), nd);
        int th = (h < w ? h : w) / 160; if (th < 2) th = 2; int sc = (h < w ? h : w) / 300; if (sc < 1) sc = 1;
        for (int i = 0; i < nd; i++) {
            float px1 = (det[i].x1 * NET_W - ox) / nw * w, px2 = (det[i].x2 * NET_W - ox) / nw * w, py1 = (det[i].y1 * NET_W - oy) / nh * h, py2 = (det[i].y2 * NET_W - oy) / nh * h;
            int x1 = (int)fmaxf(0, px1), y1 = (int)fmaxf(0, py1), x2 = (int)fminf(w - 1, px2), y2 = (int)fminf(h - 1, py2);
            int sm = (int)(det[i].score * 1000.f + 0.5f);
            app_cat(txt, sizeof txt, &tl, "%s %d.%03d  x1=%d y1=%d x2=%d y2=%d\n", COCO[det[i].cls], sm / 1000, sm % 1000, x1, y1, x2, y2);
            const uint8_t *col = PAL[det[i].cls & 7];
            for (int t = 0; t < th; t++) { for (int x = x1; x <= x2; x++) { put_px(rgb, w, h, x, y1 + t, col); put_px(rgb, w, h, x, y2 - t, col); }
                                           for (int y = y1; y <= y2; y++) { put_px(rgb, w, h, x1 + t, y, col); put_px(rgb, w, h, x2 - t, y, col); } }
            char lab[48]; snprintf(lab, sizeof lab, "%s %d.%02d", COCO[det[i].cls], sm / 1000, (sm % 1000) / 10);
            int lw = (int)strlen(lab) * 8 * sc, lh = 12 * sc, ly = y1 - lh >= 0 ? y1 - lh : y1;
            static const uint8_t blk[3] = {0, 0, 0};
            fill_rect(rgb, w, h, x1, ly, x1 + lw, ly + lh - 1, col); draw_text(rgb, w, h, x1, ly, lab, sc, blk);
        }
        uint64_t b0 = hw->now_us();
        if (!cfg.production) { snprintf(path, sizeof path, "%sout/%s.txt", root, base); fs_write(path, txt, tl); }
        snprintf(path, sizeof path, "%sout/%s_det.bmp", root, base); int rb = write_bmp(path, rgb, w, h);
        uint64_t te = hw->now_us() - b0;
        app_cat(report, sizeof report, &rl, "%s,%d,%d,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%u.%03u,%d\n", names[im], w, h, MS(tr), MS(td), MS(tp), MS(tn), MS(tb), MS(tt), MS(te), nd);
        if (cfg.production) printf("%s : %d detection(s)%s\r\n", names[im], nd, rb ? " (BMP non ecrit : memoire)" : "");
        else printf("%s : %dx%d, %d detection(s), total %u.%03u ms%s\r\n", names[im], w, h, nd, MS(tt), rb ? " (BMP non ecrit : memoire)" : "");
    }
    if (cfg.production) { printf("=== Fin : images annotees dans out/ ===\r\n"); return 0; }
    snprintf(path, sizeof path, "%sout/resultats.csv", root); fs_write(path, report, rl);
    snprintf(path, sizeof path, "%sout/benchmark.csv", root); fs_write(path, bench, bl);

    /* ---- resume : moyenne sur les images, acceleration par rapport au premier mode ---- */
    char rs[2048]; size_t rsl = 0;
    app_cat(rs, sizeof rs, &rsl, "Resume : %d image(s), %d repetition(s) par mode\n", ni, cfg.repeat);
    double ref = 0;
    for (int mi = 0; mi < cfg.nmodes; mi++) {
        const int m = cfg.modes[mi]; if (!acc[m].n) continue;
        double mean = acc[m].sum_mean_ms / acc[m].n; if (mi == 0) ref = mean;
        int ms100 = (int)(mean * 1000.0 + 0.5), rt = ref > 0 ? (int)(mean / ref * 100.0 + 0.5) : 100;
        app_cat(rs, sizeof rs, &rsl, "  %-6s : reseau moyen %d.%03d ms (%d.%02d x le temps du mode %s)\n", MODE_NAME[m], ms100 / 1000, ms100 % 1000, rt / 100, rt % 100, MODE_NAME[cfg.modes[0]]);
    }
    snprintf(path, sizeof path, "%sout/resume.txt", root); fs_write(path, rs, rsl);
    printf("%s=== Fin : resultats dans out/ ===\r\n", rs);
    return 0;
}
