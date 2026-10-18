#!/usr/bin/env python3
"""
make_figures.py - genere les figures du rapport (PDF vectoriel + PNG) et un fichier de chiffres cles.

Utilisation (PowerShell) :
    cd C:\\Users\\USER\\Desktop\\yolo_hw\\sw_sd
    python make_figures.py
Options (toutes facultatives, les valeurs par defaut cherchent les fichiers les plus recents dans yolo_hw\\logs) :
    --root     C:\\Users\\USER\\Desktop\\yolo_hw
    --results  dossier contenant benchmark.csv et resultats.csv (copie de out\\ de la carte SD)
    --couches  log serie du test des 21 couches (lignes "CSV,id,M,K,N,ip_us,...")
    --e2e      log serie du reseau complet avec lignes "LAYER,..." (programme yolo_run sur JTAG)
    --vivado   rapport vivado_util.rpt
    --puissance  mesures_puissance.csv  (colonnes : mode,puissance_w   avec mode = repos | arm | ip)
    --q8dt-ms  temps des produits matriciels du noyau 2 coeurs de la Q2 (defaut 1386 ; 0 = ne pas le montrer)
Dependances : python 3, numpy, matplotlib.
"""
import argparse, csv, glob, os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---------------------------------------------------------------- style
C = {"quant": "#56B4E9", "ip": "#D55E00", "flush": "#999999", "post": "#009E73", "autres": "#CC79A7",
     "decode": "#E69F00", "pre": "#F0E442", "bleu": "#0072B2"}
plt.rcParams.update({
    "font.size": 8, "axes.titlesize": 8.5, "axes.labelsize": 8, "legend.fontsize": 7, "xtick.labelsize": 7.5, "ytick.labelsize": 7.5,
    "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.alpha": 0.25, "grid.linewidth": 0.5,
    "pdf.fonttype": 42, "ps.fonttype": 42, "savefig.bbox": "tight", "figure.dpi": 150, "axes.axisbelow": True,
})
W1, W2 = 3.45, 7.1      # largeur colonne simple / double (pouces, format IEEE)

ap = argparse.ArgumentParser()
ap.add_argument("--root", default=r"C:\Users\USER\Desktop\yolo_hw")
ap.add_argument("--results"); ap.add_argument("--couches"); ap.add_argument("--e2e"); ap.add_argument("--vivado"); ap.add_argument("--puissance")
ap.add_argument("--q8dt-ms", type=float, default=1386.0)
ap.add_argument("--out")
A = ap.parse_args()
LOGS = os.path.join(A.root, "logs"); OUT = A.out or os.path.join(A.root, "rapport_figures"); os.makedirs(OUT, exist_ok=True)
KEY = []     # lignes du fichier de chiffres cles
def note(s): print(s); KEY.append(s)

def newest(pattern, must=None):
    files = sorted(glob.glob(pattern), key=os.path.getmtime, reverse=True)
    for f in files:
        if must is None: return f
        try:
            t = open(f, errors="ignore").read()
        except OSError: continue
        if all(m in t for m in must): return f
    return None
def save(fig, name):
    for ext in ("pdf", "png"): fig.savefig(os.path.join(OUT, f"{name}.{ext}"), dpi=300 if ext == "png" else None)
    plt.close(fig); print("  figure :", name)
def short(n, k=14): n = os.path.splitext(n)[0]; return n if len(n) <= k else n[:k - 1] + "…"

# ---------------------------------------------------------------- donnees
rdir = A.results or None
if not rdir:
    cands = sorted([d for d in glob.glob(os.path.join(LOGS, "resultats_*")) if os.path.exists(os.path.join(d, "benchmark.csv"))], key=os.path.getmtime, reverse=True)
    rdir = cands[0] if cands else None
bench, res = [], []
if rdir and os.path.exists(os.path.join(rdir, "benchmark.csv")):
    bench = list(csv.DictReader(open(os.path.join(rdir, "benchmark.csv"), encoding="utf-8-sig")))
if rdir and os.path.exists(os.path.join(rdir, "resultats.csv")):
    res = list(csv.DictReader(open(os.path.join(rdir, "resultats.csv"), encoding="utf-8-sig")))
note(f"Dossier de resultats : {rdir}  ({len(bench)} lignes benchmark, {len(res)} lignes resultats)")
fl = lambda r, k: float(r[k])

clog = A.couches or newest(os.path.join(LOGS, "serie_*.txt"), must=["BILAN : TOUT OK", "CSV,0,"])
elog = A.e2e or newest(os.path.join(LOGS, "serie_*.txt"), must=["LAYER,0,", "IDENTIQUE"])
note(f"Log test des couches : {clog}"); note(f"Log reseau complet (LAYER) : {elog}")
layers = []
if clog:
    for l in open(clog, errors="ignore"):
        p = l.strip().split(",")
        if p[0] == "CSV" and len(p) >= 6 and int(p[1]) >= 0:
            layers.append(dict(id=int(p[1]), M=int(p[2]), K=int(p[3]), N=int(p[4]), ip_us=float(p[5])))
e2e = []
if elog:
    for l in open(elog, errors="ignore"):
        m = re.match(r"LAYER,(\d+),quant_us,(\d+),flush_us,(\d+),ip_us,(\d+),post_us,(\d+)", l.strip())
        if m: e2e.append([int(x) for x in m.groups()])

modes = {}
for m in ("ip", "neon", "neon2"):
    rows = [r for r in bench if r["mode"] == m]
    if not rows: continue
    g = lambda k: float(np.mean([fl(r, k) for r in rows]))
    modes[m] = dict(n=len(rows), total=g("reseau_moy_ms"), quant=g("quant_im2col_ms"), gemm=g("produits_matriciels_ms"), flush=g("flush_ms"), post=g("post_ms"),
                    autres=g("autres_ms"), sd_max=max(fl(r, "reseau_ecart_ms") for r in rows), reps=int(rows[0]["repetitions"]))

# ---------------------------------------------------------------- fig 1 : decomposition du temps du reseau
BOTH = "ip" in modes and "neon" in modes
if BOTH:
    ORDER = ["neon"] + (["neon2"] if "neon2" in modes else []) + ["ip"]
    fig, ax = plt.subplots(figsize=(W1, 1.75 if len(ORDER) == 2 else 2.1))
    names = {"neon": "ARM seul\n(NEON, 1 cœur)", "neon2": "ARM seul\n(NEON, 2 cœurs)", "ip": "ARM + IP\n(FPGA)"}
    parts = [("quant", "Quantification + im2col (ARM)", "quant"), ("gemm", "Produits matriciels", "ip"), ("flush", "Flush du cache", "flush"),
             ("post", "Post-traitement (ARM)", "post"), ("autres", "Routes / maxpool / upsample", "autres")]
    for yi, m in enumerate(ORDER):
        left = 0
        for key, lab, col in parts:
            v = modes[m][key]; ax.barh(yi, v, left=left, color=C[col], edgecolor="white", linewidth=0.4, label=lab if yi == 0 else None)
            if v > 0.06 * max(modes["neon"]["total"], 1): ax.text(left + v / 2, yi, f"{v:.0f}", ha="center", va="center", fontsize=6.5, color="white" if col in ("ip", "post") else "black")
            left += v
        ax.text(left + 40, yi, f"{left:.0f} ms", va="center", fontsize=7.5, fontweight="bold")
    ax.set_yticks(range(len(ORDER))); ax.set_yticklabels([names[m] for m in ORDER]); ax.set_xlabel("Temps du réseau YOLOv4-tiny (ms), moyenne sur %d images" % modes["ip"]["n"])
    ax.set_xlim(0, modes["neon"]["total"] * 1.17); ax.grid(axis="y", visible=False); ax.invert_yaxis()
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.42), ncol=2, frameon=False, handlelength=1.2, columnspacing=1)
    save(fig, "fig_decomposition_reseau")

# ---------------------------------------------------------------- chiffres de synthese + fig 2 : accelerations
if BOTH:
    ip, ne = modes["ip"], modes["neon"]
    S_e2e = ne["total"] / ip["total"]; S_gemm = ne["gemm"] / ip["gemm"]
    f = ne["gemm"] / ne["total"]; S_inf = 1 / (1 - f); S_amd = 1 / ((1 - f) + f / S_gemm)
    note(f"Reseau complet : ARM seul {ne['total']:.1f} ms ; ARM+IP {ip['total']:.1f} ms ; acceleration x{S_e2e:.2f}")
    note(f"Produits matriciels : ARM seul {ne['gemm']:.1f} ms ; IP {ip['gemm']:.1f} ms ; acceleration x{S_gemm:.2f}")
    note(f"Part des produits matriciels dans le temps ARM seul : {100*f:.1f} % ; limite d'Amdahl x{S_inf:.2f} ; Amdahl a x{S_gemm:.1f} : x{S_amd:.2f}")
    note(f"Part de l'IP dans le temps ARM+IP : {100*ip['gemm']/ip['total']:.1f} % ; ARM (quant+post+autres) : {100*(ip['quant']+ip['post']+ip['autres'])/ip['total']:.1f} %")
    note(f"Ecart-type maximal du temps du reseau sur {ip['reps']} repetitions : IP {ip['sd_max']:.2f} ms ; NEON {ne['sd_max']:.2f} ms")
    if layers:
        macs = sum(l["M"] * l["K"] * l["N"] for l in layers); tus = sum(l["ip_us"] for l in layers)
        note(f"Multiplications-accumulations (21 couches) : {macs/1e9:.3f} G ; IP (test couches) : {tus/1e3:.1f} ms = {macs/tus/1e3:.2f} GMAC/s ; NEON : {macs/ne['gemm']/1e6:.2f} GMAC/s")
    labs = ["Produits\nmatriciels\nvs 1 cœur", "Réseau\ncomplet\nvs 1 cœur"]; vals = [S_gemm, S_e2e]; hat = [False, False]
    if "neon2" in modes:
        n2 = modes["neon2"]; S_g2 = n2["gemm"] / ip["gemm"]; S_e2 = n2["total"] / ip["total"]
        labs += ["Produits\nmatriciels\nvs 2 cœurs", "Réseau\ncomplet\nvs 2 cœurs"]; vals += [S_g2, S_e2]; hat += [False, False]
        note(f"ARM seul 2 coeurs (mesure) : reseau {n2['total']:.1f} ms ; produits matriciels {n2['gemm']:.1f} ms ; gain vs 1 coeur : reseau x{ne['total']/n2['total']:.2f}, produits matriciels x{ne['gemm']/n2['gemm']:.2f} (ideal x2)")
        note(f"IP contre ARM 2 coeurs (mesure) : produits matriciels x{S_g2:.2f} ; reseau complet x{S_e2:.2f}")
        if A.q8dt_ms > 0: note(f"(rappel : le noyau 2 coeurs de la Q2 donnait {A.q8dt_ms:.0f} ms pour les produits matriciels ; ici {n2['gemm']:.0f} ms avec le noyau aux formats de l'IP)")
    elif A.q8dt_ms > 0:
        S_g2 = A.q8dt_ms / ip["gemm"]; S_e2 = (ne["total"] - ne["gemm"] + A.q8dt_ms) / ip["total"]
        labs += ["Produits\nmatriciels\nvs 2 cœurs (Q2)", "Réseau\ncomplet\nvs 2 cœurs\n(estimation)"]; vals += [S_g2, S_e2]; hat += [False, True]
        note(f"Contre le noyau 2 coeurs de la Q2 ({A.q8dt_ms:.0f} ms) : produits matriciels x{S_g2:.2f} ; reseau complet x{S_e2:.2f} (ESTIMATION : on remplace le temps NEON 1 coeur par {A.q8dt_ms:.0f} ms)")
    fig, ax = plt.subplots(figsize=(W1, 2.15))
    cols = [C["ip"], C["bleu"], C["ip"], C["bleu"]][:len(vals)]
    for i, (v, h) in enumerate(zip(vals, hat)):
        ax.bar(i, v, color=cols[i], hatch="///" if h else None, edgecolor="white" if not h else "black", linewidth=0.6, width=0.7)
        ax.text(i, v + 0.15, f"×{v:.1f}", ha="center", fontsize=8, fontweight="bold")
    ax.axhline(1, color="black", lw=0.7); ax.set_xticks(range(len(vals))); ax.set_xticklabels(labs, fontsize=6.5); ax.set_ylabel("Accélération de l'IP (×)")
    ax.set_ylim(0, max(vals) * 1.18); ax.grid(axis="x", visible=False)
    save(fig, "fig_accelerations")

    # loi d'Amdahl
    s = np.logspace(0, 2, 200); fig, ax = plt.subplots(figsize=(W1, 2.1))
    ax.plot(s, 1 / ((1 - f) + f / s), color=C["bleu"], lw=1.6, label=f"Loi d'Amdahl (f = {100*f:.0f} %)")
    ax.axhline(S_inf, color="black", ls="--", lw=0.8); ax.text(1.1, S_inf + 0.08, f"limite ×{S_inf:.1f}", fontsize=7)
    ax.plot([S_gemm], [S_e2e], "o", color=C["ip"], ms=6, label=f"Mesure (×{S_gemm:.1f} → ×{S_e2e:.1f})")
    ax.plot([S_gemm], [S_amd], "x", color="black", ms=6, label=f"Amdahl idéal (×{S_amd:.2f})")
    ax.set_xscale("log"); ax.set_xlabel("Accélération des produits matriciels (×)"); ax.set_ylabel("Accélération du réseau complet (×)"); ax.legend(loc="lower right", frameon=False)
    save(fig, "fig_amdahl")

# ---------------------------------------------------------------- fig 4 : GMAC/s par couche (test des 21 couches)
if layers:
    layers.sort(key=lambda l: l["id"]); ids = list(range(1, len(layers) + 1)); g = [l["M"] * l["K"] * l["N"] / l["ip_us"] / 1e3 for l in layers]   # convolution n° 1..21
    fig, ax = plt.subplots(figsize=(W2, 2.3))
    ax.bar(range(len(ids)), g, color=C["ip"], width=0.7); ax.axhline(12.8, color="black", ls="--", lw=0.9); ax.text(len(ids) - 0.5, 13.0, "pic théorique 12,8 GMAC/s (128 MAC × 100 MHz)", ha="right", va="bottom", fontsize=7)
    mean = sum(l["M"] * l["K"] * l["N"] for l in layers) / sum(l["ip_us"] for l in layers) / 1e3
    ax.axhline(mean, color=C["bleu"], ls=":", lw=1.2); ax.text(-0.4, mean + 0.25, f"moyenne pondérée {mean:.1f} GMAC/s", fontsize=7, color=C["bleu"], va="bottom", bbox=dict(fc="white", ec="none", alpha=0.85, pad=1))
    ax.set_xticks(range(len(ids))); ax.set_xticklabels([f"{n}\n{l['K']}" for n, l in zip(ids, layers)], fontsize=6)
    ax.set_xlabel("Convolution n° (en haut) et profondeur K du produit matriciel (en bas)"); ax.set_ylabel("Débit de l'IP (GMAC/s)"); ax.set_ylim(0, 15); ax.grid(axis="x", visible=False)
    save(fig, "fig_gmacs_par_couche")
    note(f"Debit IP par couche : min {min(g):.2f} (convolution n°{ids[int(np.argmin(g))]}) ; max {max(g):.2f} (convolution n°{ids[int(np.argmax(g))]}) GMAC/s ; moyenne ponderee {mean:.2f}")

# ---------------------------------------------------------------- fig 5 : temps par couche (reseau complet)
if e2e:
    a = np.array(e2e, dtype=float); ids = a[:, 0].astype(int) + 1        # convolution n° 1..21
    fig, ax = plt.subplots(figsize=(W2, 2.4)); bottom = np.zeros(len(ids))
    for col, key, lab in ((1, "quant", "Quantification + im2col (ARM)"), (3, "ip", "Produits matriciels (IP)"), (2, "flush", "Flush du cache"), (4, "post", "Post-traitement (ARM)")):
        v = a[:, col] / 1000.0; ax.bar(range(len(ids)), v, bottom=bottom, color=C[key], label=lab, width=0.75); bottom += v
    ax.set_xticks(range(len(ids))); ax.set_xticklabels(ids, fontsize=6.5); ax.set_xlabel("Convolution n°"); ax.set_ylabel("Temps (ms)"); ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, ncol=2, loc="upper right"); save(fig, "fig_temps_par_couche")
    note(f"Couches : quant+im2col total {a[:,1].sum()/1e3:.1f} ms ; flush {a[:,2].sum()/1e3:.1f} ; IP {a[:,3].sum()/1e3:.1f} ; post {a[:,4].sum()/1e3:.1f}")

# ---------------------------------------------------------------- fig 6 : chaine complete par image
if res:
    fig, ax = plt.subplots(figsize=(W1, 2.2)); names = [short(r["image"]) for r in res]; bottom = np.zeros(len(res))
    for key, lab, col in (("decodage_ms", "Décodage JPEG", "decode"), ("pretraitement_ms", "Redim. + letterbox", "pre"), ("reseau_ms", "Réseau (ARM + IP)", "ip"), ("decodage_boites_ms", "Décodage boîtes + NMS", "autres")):
        v = np.array([fl(r, key) for r in res]); ax.bar(range(len(res)), v, bottom=bottom, color=C[col], label=lab, width=0.7, edgecolor="white", linewidth=0.3); bottom += v
    for i, t in enumerate(bottom): ax.text(i, t + 20, f"{t:.0f}", ha="center", fontsize=7)
    ax.set_xticks(range(len(res))); ax.set_xticklabels(names, rotation=30, ha="right", fontsize=6.5); ax.set_ylabel("Temps (ms)"); ax.set_ylim(0, bottom.max() * 1.15); ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, ncol=2, loc="upper center", bbox_to_anchor=(0.5, -0.38)); save(fig, "fig_chaine_complete")
    tt = [fl(r, "traitement_total_ms") for r in res]
    note(f"Chaine complete (decodage -> detections), mode IP : {np.mean(tt):.0f} ms en moyenne sur {len(res)} images (min {min(tt):.0f}, max {max(tt):.0f})")

# ---------------------------------------------------------------- fig 7 : repetabilite
if bench and BOTH:
    imgs = list(dict.fromkeys(r["image"] for r in bench)); fig, ax = plt.subplots(figsize=(W1, 2.2)); w = 0.8 / (3 if "neon2" in modes else 2)
    LST = [("neon", "ARM seul (NEON, 1 cœur)", C["bleu"])] + ([("neon2", "ARM seul (2 cœurs)", C["post"])] if "neon2" in modes else []) + [("ip", "ARM + IP", C["ip"])]
    for k, (m, lab, col) in enumerate(LST):
        sel = [next(r for r in bench if r["image"] == i and r["mode"] == m) for i in imgs if any(r["image"] == i and r["mode"] == m for r in bench)]
        y = np.array([fl(r, "reseau_moy_ms") for r in sel]); lo = y - np.array([fl(r, "reseau_min_ms") for r in sel]); hi = np.array([fl(r, "reseau_max_ms") for r in sel]) - y
        ax.bar(np.arange(len(sel)) + (k - (len(LST) - 1) / 2) * w, y, w, color=col, label=lab, yerr=[lo, hi], capsize=2, error_kw=dict(lw=0.8))
    ax.set_xticks(range(len(imgs))); ax.set_xticklabels([short(i, 10) for i in imgs], rotation=30, ha="right", fontsize=6.5)
    ax.set_ylabel("Temps du réseau (ms), min–max"); ax.set_ylim(0, ax.get_ylim()[1] * 1.22); ax.legend(frameon=False, loc="upper right", ncol=2); ax.grid(axis="x", visible=False); save(fig, "fig_repetabilite")
    note("Les barres d'erreur (min-max sur %d repetitions) sont quasi invisibles : le temps est tres reproductible." % (modes.get("ip", {}).get("reps", 0)))

# ---------------------------------------------------------------- fig 8 : occupation du FPGA
vp = A.vivado or os.path.join(LOGS, "vivado_util.rpt")
if os.path.exists(vp):
    want = {"Slice LUTs": "LUT", "Slice Registers": "Registres", "Block RAM Tile": "BRAM", "DSPs": "DSP"}; got = {}
    for l in open(vp, errors="ignore"):
        if "|" not in l: continue
        c = [x.strip() for x in l.strip().strip("|").split("|")]
        for k, lab in want.items():
            if c and c[0].replace("*", "").strip() == k and lab not in got:
                try: got[lab] = (float(c[1]), float(c[-2]), float(c[-1].replace("<", "")))
                except (ValueError, IndexError): pass
    if got:
        fig, ax = plt.subplots(figsize=(W1, 1.8)); labs = list(got)
        for i, k in enumerate(labs):
            used, avail, pct = got[k]; ax.barh(i, pct, color=C["ip"]); ax.text(pct + 1, i, f"{used:.0f} / {avail:.0f}  ({pct:.1f} %)", va="center", fontsize=7)
        ax.set_yticks(range(len(labs))); ax.set_yticklabels(labs); ax.invert_yaxis(); ax.set_xlim(0, 100); ax.set_xlabel("Occupation du Zynq-7020 (%)"); ax.grid(axis="y", visible=False)
        save(fig, "fig_ressources_fpga"); note("Ressources FPGA : " + " ; ".join(f"{k} {v[0]:.0f}/{v[1]:.0f} ({v[2]:.1f} %)" for k, v in got.items()))
    else: note("vivado_util.rpt present mais aucune ligne reconnue")
else: note("(pas de vivado_util.rpt : figure des ressources non generee)")

# ---------------------------------------------------------------- fig 9 : energie par image
pp = A.puissance or os.path.join(LOGS, "mesures_puissance.csv")
if os.path.exists(pp) and BOTH:
    P = {r["mode"]: float(r["puissance_w"]) for r in csv.DictReader(open(pp, encoding="utf-8-sig"))}
    if "arm" in P and "ip" in P:
        E = {"neon": P["arm"] * modes["neon"]["total"] / 1000, "ip": P["ip"] * modes["ip"]["total"] / 1000}
        fig, ax = plt.subplots(figsize=(W1, 1.9))
        for i, (m, lab, col) in enumerate((("neon", "ARM seul\n(NEON)", C["bleu"]), ("ip", "ARM + IP", C["ip"]))):
            ax.bar(i, E[m], color=col, width=0.55); ax.text(i, E[m] + 0.05 * max(E.values()), f"{E[m]:.2f} J", ha="center", fontsize=8, fontweight="bold")
        ax.set_xticks([0, 1]); ax.set_xticklabels(["ARM seul\n(NEON)", "ARM + IP"]); ax.set_ylabel("Énergie par image (J)"); ax.set_ylim(0, max(E.values()) * 1.2); ax.grid(axis="x", visible=False)
        save(fig, "fig_energie"); note(f"Energie par image : ARM seul {E['neon']:.2f} J ({P['arm']:.2f} W) ; ARM+IP {E['ip']:.2f} J ({P['ip']:.2f} W) ; rapport x{E['neon']/E['ip']:.2f}")
elif BOTH:
    note("(pas de mesures_puissance.csv : figure de l'energie non generee ; modele : colonnes mode,puissance_w avec repos / arm / ip)")

open(os.path.join(OUT, "chiffres_cles.txt"), "w", encoding="utf-8").write("\n".join(KEY) + "\n")
print("\nFichiers dans :", OUT)
