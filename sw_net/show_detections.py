# python show_detections.py <log_serie.txt> <image.jpg> [sortie.png]  -- dessine les lignes "DET," du log
import sys
from PIL import Image, ImageDraw
log, im = sys.argv[1], sys.argv[2]; out = sys.argv[3] if len(sys.argv) > 3 else "detections.png"
img = Image.open(im).convert("RGB"); W, H = img.size
s = min(416 / W, 416 / H); nw, nh = round(W * s), round(H * s); ox, oy = (416 - nw) // 2, (416 - nh) // 2  # letterbox
d = ImageDraw.Draw(img)
for l in open(log, errors="ignore"):
    p = l.strip().split(",")
    if p[0] != "DET" or len(p) < 14: continue
    name, sc = p[2], int(p[4]) / 1000
    x1, y1, x2, y2 = (int(p[i]) for i in (6, 8, 10, 12))
    x1, x2 = (x1 - ox) / s, (x2 - ox) / s; y1, y2 = (y1 - oy) / s, (y2 - oy) / s
    d.rectangle([x1, y1, x2, y2], outline="red", width=3); d.text((x1 + 3, y1 + 3), f"{name} {sc:.2f}", fill="yellow")
    print(name, sc)
img.save(out); print("->", out)
