import json, sys, bisect
ocr = json.load(open(sys.argv[1])); ev = json.load(open(sys.argv[2])); ours = json.load(open(sys.argv[3], encoding="utf-8"))
# bar pixel -> percent, calibrated on the four frames whose percent was read by eye
cal = [(20, 18.33), (30, 34.95), (40, 51.05), (70, 97.56)]
xs = [ocr[s*60]["bar"] for s, _ in cal]
print("bar at cal:", list(zip(xs, [p for _, p in cal])))
# least squares line
n = len(xs); mx = sum(xs)/n; my = sum(p for _, p in cal)/n
a = sum((x-mx)*(p-my) for x, (_, p) in zip(xs, cal)) / sum((x-mx)**2 for x in xs); b = my - a*mx
print("pct = %.4f*bar + %.3f" % (a, b), "residuals", [round(a*x+b-p, 2) for x, (_, p) in zip(xs, cal)])
# reference events with percent: use the median bar over the event frame +-3 (bar is noisy when occluded)
def bar_at(f):
    v = sorted(ocr[j]["bar"] for j in range(max(0,f-3), min(len(ocr), f+4)) if ocr[j]["bar"] > 0)
    return v[len(v)//2] if v else -1
REF = [(a*bar_at(e["frame"])+b, e["row"], e["frame"]) for e in ev]
def bucket(w):
    w = int(round(w))
    if w <= 1: return "1"
    if w <= 4: return str(w)
    if w <= 6: return "5-6"
    if w <= 8: return "7-8"
    if w <= 10: return "9-10"
    return "11+"
OUR = [(i["percent"], bucket(i["window"]), i["index"], i["window"], i.get("trace",""), i["mode"]) for i in ours["inputs"] if i["type"] == "press" and i["status"] in ("measured","capped")]
# monotone alignment: each ref event matched to one of our clicks (in order), our clicks may be skipped (cost: skipping is free
# for 11+ clicks, small otherwise); ref events may be skipped at a high cost.
INF = 1e18; R, O = len(REF), len(OUR)
SKIP_OUR = lambda o: 0.15 if OUR[o][1] == "11+" else 0.6
SKIP_REF = 2.0
D = [[INF]*(O+1) for _ in range(R+1)]; P = [[None]*(O+1) for _ in range(R+1)]
D[0][0] = 0
for r in range(R+1):
    for o in range(O+1):
        d = D[r][o]
        if d >= INF: continue
        if r < R and o < O:
            c = d + abs(REF[r][0] - OUR[o][0]) * 1.0
            if c < D[r+1][o+1]: D[r+1][o+1] = c; P[r+1][o+1] = (r, o, "m")
        if o < O:
            c = d + SKIP_OUR(o)
            if c < D[r][o+1]: D[r][o+1] = c; P[r][o+1] = (r, o, "so")
        if r < R:
            c = d + SKIP_REF
            if c < D[r+1][o]: D[r+1][o] = c; P[r+1][o] = (r, o, "sr")
r, o = R, O; pairs = []; skipped_our = []; skipped_ref = []
while (r, o) != (0, 0):
    pr, po, t = P[r][o]
    if t == "m": pairs.append((pr, po))
    elif t == "so": skipped_our.append(po)
    else: skipped_ref.append(pr)
    r, o = pr, po
pairs.reverse()
ORDER = ["1","2","3","4","5-6","7-8","9-10","11+"]
idx = {k: i for i, k in enumerate(ORDER)}
print("matched", len(pairs), "our skipped", len(skipped_our), "ref skipped", len(skipped_ref))
from collections import Counter
print("our skipped buckets:", Counter(OUR[o][1] for o in skipped_our))
diffs = Counter(); rows = []
for rr, oo in pairs:
    d = idx[OUR[oo][1]] - idx[REF[rr][1]]
    diffs[d] += 1; rows.append((d, rr, oo))
print("bucket difference (ours - ref):", sorted(diffs.items()))
print("mean |pct diff|:", sum(abs(REF[rr][0]-OUR[oo][0]) for rr, oo in pairs)/len(pairs))
out = []
for d, rr, oo in rows:
    out.append({"diff": d, "ref": REF[rr][1], "refpct": round(REF[rr][0],2), "ours": OUR[oo][1], "ourpct": round(OUR[oo][0],2), "index": OUR[oo][2], "window": OUR[oo][3], "trace": OUR[oo][4], "mode": OUR[oo][5]})
json.dump(out, open(sys.argv[4], "w"), indent=0)
low = [x for x in out if x["diff"] <= -2]
print("ours >=2 buckets LOWER than ref:", len(low))
for x in low: print(x)
