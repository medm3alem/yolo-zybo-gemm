#!/usr/bin/env python3
"""Genere pour la carte : model.bin (poids int8 par filtre, deja au format de l'IP), net_def.h (structure du reseau),
img.bin (image letterbox 416x416 float32 CHW) et la reference numpy exacte (heads_ref.bin, det_ref.txt).
Pipeline int8 (identique a l'ARM) : poids int8 par filtre (q8pc), activations quantifiees a la volee (echelle = max|x| de la couche),
GEMM int32 exact, puis rescale float32 + biais + leaky."""
import sys, struct, numpy as np
from yolo_np import load_net, im2col, decode, letterbox, sigmoid
from PIL import Image
sys.path.insert(0, '.')
import importlib.util
spec = importlib.util.spec_from_file_location('gp', 'pack_py.py'); 
cfg, wts, img_path = sys.argv[1], sys.argv[2], sys.argv[3]
out = sys.argv[4] if len(sys.argv) > 4 else '.'
f32 = np.float32

def pack_A(qA, M, K):                      # (M,K) int8 -> octets : [mt][k][8]
    MT = (M + 7) // 8
    pad = np.zeros((MT * 8, K), np.int8); pad[:M] = qA
    return np.ascontiguousarray(pad.reshape(MT, 8, K).transpose(0, 2, 1)).tobytes()

layers = load_net(cfg, wts)
x0 = letterbox(Image.open(img_path)).astype(f32)

# ---- quantification des poids : par filtre, symetrique, +-127 ----
conv_idx = {}
for i, L in enumerate(layers):
    if L['type'] == 'convolutional':
        W = L['W'].astype(np.float64)
        sA = 127.0 / np.maximum(np.abs(W).max(axis=1), 1e-30)
        L['qA'] = np.rint(W * sA[:, None]).astype(np.int8)
        L['inv_sA'] = (1.0 / sA).astype(f32)
        conv_idx[i] = len(conv_idx)

def forward(x, mode):
    """mode 'float' ou 'int8'. Retourne sorties de chaque couche et les heads."""
    outs = []; heads = []; cur = x; shapes = []
    for i, L in enumerate(layers):
        t = L['type']
        if t == 'convolutional':
            B, Ho, Wo = im2col(cur, L['k'], L['stride'])
            if mode == 'float':
                C = (L['W'] @ B).astype(f32)
            else:
                mx = f32(np.abs(cur).max())
                sB = f32(127.0) / mx if mx > 0 else f32(1.0)
                invsB = mx / f32(127.0) if mx > 0 else f32(1.0)
                qB = np.clip(np.rint(B.astype(f32) * sB), -127, 127)
                Ci = L['qA'].astype(np.float64) @ qB.astype(np.float64)         # exact (< 2^53)
                assert np.abs(Ci).max() < 2**31
                scale = (L['inv_sA'] * invsB).astype(f32)                       # float32 par filtre
                C = Ci.astype(f32) * scale[:, None]
            C = C + L['b'][:, None]
            if L['leaky']: C = np.where(C > 0, C, f32(0.1) * C)
            cur = C.reshape(L['n'], Ho, Wo).astype(f32)
        elif t == 'route':
            ts = [outs[j] for j in L['idx']]
            if L['groups'] > 1:
                c = ts[0].shape[0] // L['groups']; ts = [ts[0][L['gid'] * c:(L['gid'] + 1) * c]]
            cur = np.concatenate(ts, axis=0)
        elif t == 'maxpool':
            C_, H, W = cur.shape; cur = cur.reshape(C_, H // 2, 2, W // 2, 2).max(axis=(2, 4))
        elif t == 'upsample':
            cur = cur.repeat(2, axis=1).repeat(2, axis=2)
        elif t == 'yolo':
            heads.append((cur.copy(), L))
        outs.append(cur)
    return outs, heads

outs_f, heads_f = forward(x0, 'float')
outs_q, heads_q = forward(x0, 'int8')

# ---------------- model.bin ----------------
ALIGN = 64
def al(n): return (n + ALIGN - 1) // ALIGN * ALIGN
nconv = len(conv_idx)
table_off = 64
data_off = al(table_off + nconv * 32)
blob = bytearray(data_off)
entries = []
for i, L in enumerate(layers):
    if L['type'] != 'convolutional': continue
    M, K = L['qA'].shape
    a = pack_A(L['qA'], M, K); s = L['inv_sA'].astype('<f4').tobytes(); b = L['b'].astype('<f4').tobytes()
    ent = []
    for blk in (a, s, b):
        off = al(len(blob)); blob.extend(b'\0' * (off - len(blob))); ent.append(off); blob.extend(blk)
    entries.append((M, K, *ent))
struct.pack_into('<4sI', blob, 0, b'YOLM', nconv)
for j, (M, K, ao, so, bo) in enumerate(entries):
    struct.pack_into('<8I', blob, table_off + 32 * j, M, K, ao, so, bo, 0, 0, 0)
open(f'{out}/model.bin', 'wb').write(blob)
x0.astype('<f4').tofile(f'{out}/img.bin')

# ---------------- net_def.h ----------------
ops = []; pool = 0; max_bp = 0; max_c = 0; shp_in = x0.shape
prev_shape = x0.shape; off_of = []
def alf(n): return (n + 15) // 16 * 16
for i, L in enumerate(layers):
    t = L['type']; o = outs_q[i]; ci, hi, wi = prev_shape; co, ho, wo = o.shape
    rec = dict(type={'convolutional': 0, 'route': 1, 'maxpool': 2, 'upsample': 3, 'yolo': 4}[t], ci=ci, hi=hi, wi=wi, co=co, ho=ho, wo=wo,
               k=0, s=0, leaky=0, conv=-1, nsrc=0, s0=-1, s1=-1, groups=1, gid=0, in_op=i - 1, out_off=0, head=-1)
    if t == 'convolutional':
        rec.update(k=L['k'], s=L['stride'], leaky=int(L['leaky']), conv=conv_idx[i])
        K = L['k'] * L['k'] * ci; N = ho * wo; M = co
        max_bp = max(max_bp, ((N + 15) // 16) * K * 16); max_c = max(max_c, ((M + 7) // 8) * 8 * ((N + 15) // 16) * 16)
    if t == 'route':
        rec.update(nsrc=len(L['idx']), s0=L['idx'][0], s1=L['idx'][1] if len(L['idx']) > 1 else -1, groups=L['groups'], gid=L['gid'])
    if t == 'yolo':
        rec['out_off'] = off_of[i - 1]; rec['head'] = sum(1 for r in ops if r['type'] == 4)
    else:
        rec['out_off'] = pool; pool += alf(co * ho * wo)
    off_of.append(rec['out_off']); ops.append(rec); prev_shape = o.shape

with open(f'{out}/net_def.h', 'w') as h:
    h.write('/* genere par gen_model.py - ne pas editer */\n#ifndef NET_DEF_H\n#define NET_DEF_H\n')
    h.write('typedef struct { int type, ci, hi, wi, co, ho, wo, k, s, leaky, conv, nsrc, s0, s1, groups, gid, in_op, head; unsigned out_off; } op_t;\n')
    h.write('typedef struct { int nc, nmask, mask[3]; float anchors[12]; float scale_x_y; } yolo_cfg_t;\n')
    h.write(f'#define NOPS {len(ops)}\n#define NCONV {nconv}\n#define NHEADS 2\n#define NET_W 416\n#define NET_H 416\n')
    h.write(f'#define POOL_FLOATS {pool}u\n#define BP_BYTES_MAX {max_bp}u\n#define C_WORDS_MAX {max_c}u\n')
    h.write('static const op_t OPS[NOPS] = {\n')
    for r in ops:
        h.write('  {%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%du},\n' % (r['type'], r['ci'], r['hi'], r['wi'], r['co'], r['ho'], r['wo'], r['k'], r['s'], r['leaky'], r['conv'], r['nsrc'], r['s0'], r['s1'], r['groups'], r['gid'], r['in_op'], r['head'], r['out_off']))
    h.write('};\nstatic const yolo_cfg_t YOLO[NHEADS] = {\n')
    for L in layers:
        if L['type'] == 'yolo':
            an = [float(a) for a in L['anchors'].replace(' ', '').split(',')]; mk = [int(m) for m in L['mask'].split(',')]
            h.write('  {%d,%d,{%d,%d,%d},{%s},%sf},\n' % (int(L['classes']), len(mk), mk[0], mk[1], mk[2], ','.join('%.1ff' % a for a in an), L.get('scale_x_y', '1.0')))
    h.write('};\n#endif\n')

# ---------------- references ----------------
with open(f'{out}/heads_ref.bin', 'wb') as hf:
    for hd, L in heads_q: hd.astype('<f4').tofile(hf)
def dets(heads):
    return decode(heads)
for name, hs in (('float', heads_f), ('int8', heads_q)):
    d = dets(hs)
    with open(f'{out}/det_ref_{name}.txt', 'w') as fo:
        for s, c, x1, y1, x2, y2 in d: fo.write('%d %.5f %.4f %.4f %.4f %.4f\n' % (c, s, x1, y1, x2, y2))
    print(name, len(d), 'detections')
print('pool %.1f Mo, bp max %.1f Mo, C max %.1f Mo, model.bin %.2f Mo' % (pool * 4 / 1e6, max_bp / 1e6, max_c * 4 / 1e6, len(blob) / 1e6))
# empreinte FNV-1a 64 bits des tetes (verifiee par la carte)
_w = np.fromfile(f'{out}/heads_ref.bin', '<u4'); _h = 1469598103934665603
for _x in _w.tolist(): _h ^= _x; _h = (_h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
open(f'{out}/expected.h', 'w').write('#define HEADS_WORDS %du\n#define EXPECTED_HASH_HI 0x%08xu\n#define EXPECTED_HASH_LO 0x%08xu\n' % (len(_w), _h >> 32, _h & 0xFFFFFFFF))
print('expected.h : %x' % _h)
