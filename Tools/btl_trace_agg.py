# Usage: python btl_trace_agg.py events.csv [maxDepth] -> total exclusive-ish (inclusive at depth<=maxDepth) time per timer
import sys, csv, collections
f=sys.argv[1]; md=int(sys.argv[2]) if len(sys.argv)>2 else 99
tot=collections.Counter(); cnt=collections.Counter()
with open(f, newline='', encoding='utf-8', errors='replace') as fh:
    r=csv.reader(fh); hdr=next(r)
    idx={h:i for i,h in enumerate(hdr)}
    for row in r:
        try: d=float(row[idx["Duration"]]); dp=int(row[idx["Depth"]])
        except: continue
        if dp>md: continue
        k=(row[idx["ThreadName"]] if "ThreadName" in idx else "")+"|"+str(dp)+"|"+row[idx["TimerName"]]
        tot[k]+=d; cnt[k]+=1
for k,v in tot.most_common(60): print("%8.1f ms %6d  %s" % (v*1000, cnt[k], k[:110]))
