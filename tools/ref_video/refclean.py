import json, sys, collections
res = json.load(open(sys.argv[1]))
ROWS = ["9-10","7-8","5-6","4","3","2","1"]
N = len(res)
# Find the run: frames where all rows read 0 at the start of the attempt. Use the last start before the end.
# Clean each row: value v at frame i is accepted if the same value is read in >= 3 of the 5 frames around i.
clean = {k: [None]*N for k in ROWS}
for k in ROWS:
    v = [r[k] for r in res]
    for i in range(N):
        win = [x for x in v[max(0,i-2):i+3] if x is not None]
        if not win: continue
        c, n = collections.Counter(win).most_common(1)[0]
        if n >= 3: clean[k][i] = c
# monotonic events: track the running max per row; an increase of +1 is an event at that frame. Ignore drops (glitches),
# big jumps are reported.
events = []; odd = []
for k in ROWS:
    cur = None
    for i in range(N):
        x = clean[k][i]
        if x is None: continue
        if cur is None:
            cur = x; continue
        if x == 0 and cur > 3:   # new attempt
            cur = 0; odd.append(("reset", k, i)); continue
        if x > cur:
            if x - cur > 3: odd.append(("jump", k, i, cur, x))
            for _ in range(x - cur): events.append((i, k))
            cur = x
events.sort()
print("odd:", odd[:20])
known = {20: [1,0,1,4,7,9,3], 30: [1,1,4,9,12,16,10], 40: [1,1,7,15,27,24,12], 70: [1,2,13,24,48,42,26]}
order = ["1","2","3","4","5-6","7-8","9-10"]
for s, kv in known.items():
    c = collections.Counter(k for f, k in events if f <= s*60)
    print(s, [c[k] for k in order], kv)
c = collections.Counter(k for f, k in events); print("final", [c[k] for k in order], sum(c.values()))
json.dump([{"frame": f, "row": k, "bar": res[f]["bar"]} for f, k in events], open(sys.argv[2], "w"))
