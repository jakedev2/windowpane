import sys, subprocess, json, numpy as np
video, out = sys.argv[1], sys.argv[2]
W, H = 1280, 344
ROWS = ["9-10","7-8","5-6","4","3","2","1"]
YC = dict(zip(ROWS, [24,74,122,172,222,270,320]))
def frames():
    p = subprocess.Popen(["ffmpeg","-v","error","-i",video,"-vf","crop=1280:344:0:0","-f","rawvideo","-pix_fmt","rgb24","-"], stdout=subprocess.PIPE)
    fs = W*H*3
    while True:
        b = p.stdout.read(fs)
        if len(b) < fs: return
        yield np.frombuffer(b, np.uint8).reshape(H, W, 3).astype(np.int16)
allf = list(frames())
print(len(allf), "frames", file=sys.stderr)
# row colour: median of bright label pixels in the clean end frame
clean = allf[73*60]
COL = {}
for k in ROWS:
    y = YC[k]; reg = clean[y-17:y+17, 8:118].reshape(-1,3)
    br = reg[reg.max(axis=1) > 150]
    COL[k] = np.median(br, axis=0)
def mask(f, k):
    y = YC[k]; reg = f[y-18:y+18, 120:225]
    d = np.abs(reg - COL[k]).sum(axis=2)
    return d < 90
def glyphs(m):
    cols = m.sum(axis=0) >= 2
    gl = []; x = 0; n = len(cols)
    while x < n:
        if cols[x]:
            s = x
            while x < n and cols[x]: x += 1
            if x - s >= 4: gl.append(m[:, s:x])
        else: x += 1
    out = []
    for g in gl:
        rows = np.where(g.sum(axis=1) > 0)[0]
        if len(rows) < 10: continue
        g = g[rows[0]:rows[-1]+1]
        # resize to 16x12 by sampling
        yi = (np.arange(16) * g.shape[0] / 16).astype(int); xi = (np.arange(12) * g.shape[1] / 12).astype(int)
        out.append((g[yi][:, xi].astype(np.float32), g.shape[1] / g.shape[0]))
    return out
T = {}
def learn(f, vals):
    for k, v in vals.items():
        gs = glyphs(mask(f, k))
        s = str(v)
        if len(gs) != len(s): print("learn mismatch", k, v, len(gs), file=sys.stderr); continue
        for ch, g in zip(s, gs): T.setdefault(ch, []).append(g)
learn(allf[73*60], {"9-10":27,"7-8":42,"5-6":48,"4":25,"3":13,"2":2,"1":1})
learn(allf[30*60], {"9-10":10,"7-8":16,"5-6":12,"4":9,"3":4,"2":1,"1":1})
print("templates", sorted(T), file=sys.stderr)
def read(f, k):
    gs = glyphs(mask(f, k))
    if not gs or len(gs) > 2: return None
    s = ""; worst = 0
    for g, ar in gs:
        best, bd = None, 1e9
        for ch, lst in T.items():
            for tg, tar in lst:
                dd = np.abs(g - tg).mean() + 0.5 * abs(ar - tar)
                if dd < bd: bd, best = dd, ch
        worst = max(worst, bd); s += best
    return int(s) if worst < 0.22 else None
res = []
for i, f in enumerate(allf):
    r = {"f": i}
    for k in ROWS: r[k] = read(f, k)
    bar = f[14:22, 405:875]
    yel = (bar[...,0] > 170) & (bar[...,1] > 150) & (bar[...,2] < 120)
    cols = np.where(yel.sum(axis=0) >= 3)[0]
    r["bar"] = int(cols.max()) if len(cols) else -1
    res.append(r)
json.dump(res, open(out, "w"))
