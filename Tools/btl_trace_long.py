# Usage: python btl_trace_long.py events.csv [minMs] [tmin] [tmax]
import sys, csv
f=sys.argv[1]; mn=float(sys.argv[2]) if len(sys.argv)>2 else 30; tmin=float(sys.argv[3]) if len(sys.argv)>3 else 0; tmax=float(sys.argv[4]) if len(sys.argv)>4 else 1e9
out=[]
with open(f, newline='', encoding='utf-8', errors='replace') as fh:
    r=csv.reader(fh); hdr=next(r)
    iN,iS,iD,iDp=[hdr.index(x) for x in ("TimerName","StartTime","Duration","Depth")]
    for row in r:
        try: d=float(row[iD])*1000
        except: continue
        if d<mn: continue
        s=float(row[iS])
        if s<tmin or s>tmax: continue
        out.append((s,int(row[iDp]),d,row[iN]))
out.sort()
for s,dp,d,n in out: print("%9.3f %s%-60s %7.1f ms" % (s,"  "*min(dp,20),n[:60],d))
