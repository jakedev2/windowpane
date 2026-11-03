#!/usr/bin/env python3
"""Compare a Windowpane results file (the exported .json) with the counters of a reference frame-window video.

    python tools/compare_reference.py results.json [--reference tools/reference_acheron.json] [--cap 14]

Why shares and merged buckets: the reference shows 1, 2, 3, 4, 5-6, 7-8 and 9-10 only, counts clicks (not releases), and its video
macro differs slightly from the imported file. So every window of 9 or more (and every click the reference has no bucket for)
is merged into one "9+" bucket, and the comparison is on shares of all clicks, not on single clicks.

Reading the output:
  - "<=2" is the number to watch. A human-made top-1 run has almost no 1-2 frame clicks; a measurement that reports many of them
    is usually too strict (a fixed later input, a snapshot that did not restore exactly, a hold that does not move with the click).
  - TV distance = half the sum of |share difference|; 0 = same distribution, 1 = nothing in common.
"""
import argparse, json, os, sys

BUCKETS = ["1", "2", "3", "4", "5-6", "7-8", "9+"]

def bucket(w):
    if w <= 1: return "1"
    if w == 2: return "2"
    if w == 3: return "3"
    if w == 4: return "4"
    if w <= 6: return "5-6"
    if w <= 8: return "7-8"
    return "9+"

def shares(counts, total):
    return {b: (counts.get(b, 0) / total if total else 0.0) for b in BUCKETS}

def tv(a, b):
    return 0.5 * sum(abs(a[k] - b[k]) for k in BUCKETS)

def ref_counts(c, clicks):
    """reference bucket counts with the 9-10 bucket and the uncounted clicks merged into 9+"""
    out = {k: c.get(k, 0) for k in ("1", "2", "3", "4", "5-6", "7-8")}
    counted = sum(c.values())
    out["9+"] = c.get("9-10", 0) + max(0, clicks - counted)
    return out

def row(label, counts, total):
    s = shares(counts, total)
    cells = "  ".join(f"{counts.get(b, 0):4d} ({s[b]*100:4.1f}%)" for b in BUCKETS)
    return f"{label:<12} {cells}   n={total}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--reference", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "reference_acheron.json"))
    ap.add_argument("--cap", type=int, default=0, help="ignore windows larger than this (the reference has no bucket for them); 0 = keep all")
    a = ap.parse_args()
    res = json.load(open(a.results, encoding="utf-8"))
    ref = json.load(open(a.reference, encoding="utf-8"))
    presses = [i for i in res["inputs"] if i["type"] == "press" and i.get("window") is not None and i["status"] in ("measured", "capped")]
    if not presses:
        sys.exit("no measured clicks in the results file")
    print(f"{res.get('levelName', '?')}  macro: {res.get('macro', '?')}  clicks measured: {len(presses)}  trials: {res.get('totalTrials')}  "
          f"time: {res.get('elapsedSeconds', 0):.0f}s  ticks simulated: {res.get('totalTicks')} ({(res.get('totalTicks') or 0) / max(1, res.get('totalTrials') or 1):.0f} per trial)")
    print("\nbucket       " + "  ".join(f"{b:>12}" for b in BUCKETS))

    ours = {}
    for i in presses: ours[bucket(i["window"])] = ours.get(bucket(i["window"]), 0) + 1
    rc = ref_counts(ref["final"], ref["final_clicks_cps"])
    print(row("reference", rc, ref["final_clicks_cps"]))
    print(row("this run", ours, len(presses)))
    d = tv(shares(ours, len(presses)), shares(rc, ref["final_clicks_cps"]))
    lo_o = (ours.get("1", 0) + ours.get("2", 0)) / len(presses)
    lo_r = (rc["1"] + rc["2"]) / ref["final_clicks_cps"]
    print(f"\nTV distance {d:.3f}   windows <=2: {lo_o*100:.1f}% (reference {lo_r*100:.1f}%)   "
          f"capped (hit the max shift): {sum(1 for i in presses if i['status']=='capped')}")

    print("\nsame comparison at the points where the reference was read (this run counts clicks up to that percent):")
    for cp in ref["checkpoints"]:
        p = cp["percent"]
        sub = [i for i in presses if i["percent"] <= p]
        o = {}
        for i in sub: o[bucket(i["window"])] = o.get(bucket(i["window"]), 0) + 1
        r = ref_counts(cp["counts"], cp["clicks_cps"])
        print(f"  {p:5.2f}%  TV {tv(shares(o, len(sub)), shares(r, cp['clicks_cps'])):.3f}   "
              f"<=2: this {(o.get('1',0)+o.get('2',0))/max(1,len(sub))*100:4.1f}% / ref {(r['1']+r['2'])/cp['clicks_cps']*100:4.1f}%   "
              f"9+: this {o.get('9+',0)/max(1,len(sub))*100:4.1f}% / ref {r['9+']/cp['clicks_cps']*100:4.1f}%   (n {len(sub)} / {cp['clicks_cps']})")

    # where the strictness sits: share of <=2 windows per 10% of the level
    print("\nclicks with a window <=2 per 10% of the level (count / clicks):")
    line = []
    for lo in range(0, 100, 10):
        sub = [i for i in presses if lo <= i["percent"] < lo + 10]
        line.append(f"{lo}-{lo+10}%: {sum(1 for i in sub if i['window'] <= 2)}/{len(sub)}")
    print("  " + "   ".join(line))

if __name__ == "__main__":
    main()
