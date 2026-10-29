import re, sys
from datetime import datetime
def ts(line):
    m = re.match(r"\[(\d{4})\.(\d\d)\.(\d\d)-(\d\d)\.(\d\d)\.(\d\d):(\d{3})\]", line)
    if not m: return None
    y,mo,d,h,mi,s,ms = map(int, m.groups()); return datetime(y,mo,d,h,mi,s,ms*1000).timestamp()
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
clicks = []; cur = None; hitches = []; secs = []
for i,l in enumerate(lines):
    t = ts(l)
    if "BTLB CLICK" in l or "BTLB SELECT " in l:
        cur = {"what": l.split("BTLB ")[1], "t": t, "tiles": None, "playing": None, "entries": None}
        clicks.append(cur)
    elif "BTLB HITCH" in l:
        hitches.append((t, float(re.search(r"HITCH (\d+)", l).group(1)), cur["what"] if cur else "pre"))
    elif "BTLB SEC" in l:
        secs.append((t, l.split("BTLB ")[1], cur["what"] if cur else "pre"))
    elif cur and "BongThumbLive.Status: Container" in l and t:
        m = re.search(r"tiles on screen=(\d+)", l); p = re.search(r"playing tiles=(\d+)", l); e = re.search(r"entries=(\d+)", l)
        dt = t - cur["t"]
        if m and cur["tiles"] is None and int(m.group(1)) > 3: cur["tiles"] = dt
        if p and cur["playing"] is None and int(p.group(1)) > 0: cur["playing"] = dt
        if e and cur["entries"] is None and int(e.group(1)) > 0: cur["entries"] = dt
for c in clicks:
    f = lambda v: "-" if v is None else "%.2fs" % v
    print("%-90s tiles>3 %s  firstEntry %s  firstPlaying %s" % (c["what"][:90], f(c["tiles"]), f(c["entries"]), f(c["playing"])))
from collections import defaultdict
agg = defaultdict(list)
for t,ms,w in hitches: agg[w].append(ms)
for w,v in agg.items():
    print("HITCHES %-70s n=%d max=%.0fms >100ms=%d >250ms=%d sum=%.0fms" % (w[:70], len(v), max(v), sum(1 for x in v if x>100), sum(1 for x in v if x>250), sum(v)))
for t,s,w in secs: pass
low = [s for s in secs if float(re.search(r"fps=([\d.]+)", s[1]).group(1)) < 50]
print("seconds with fps<50:", len(low), "of", len(secs))
for s in low[:40]: print("  ", s[1], "|", s[2][:60])
for l in lines:
    if "memory: editor" in l: last_mem = l
print(last_mem[-200:] if 'last_mem' in dir() else "")
