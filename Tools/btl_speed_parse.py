import re,sys
L=open(sys.argv[1],encoding="utf-8",errors="replace").read().splitlines()
st=[l for l in L if "BongThumbLive.Status: liveActive" in l]
last=st[-1] if st else ""
m=re.search(r"newTileToShown\(n=(\d+) avg=([\d.]+)s max=([\d.]+)s waiting=(\d+)",last)
own=[int(x) for l in st for x in re.findall(r"ownedAnimRoots=(\d+)",l)]
h=[float(re.search(r"HITCH (\d+)",l).group(1)) for l in L if "BTLD HITCH" in l]
secs=[int(re.search(r"fps=(\d+)",l).group(1)) for l in L if "BTLD SEC" in l]
tr=[l for l in L if "BTLT tile=" in l]
def col(k): 
    v=[float(re.search(k+r"=(-?[\d.]+)",l).group(1)) for l in tr]; return [x for x in v if x>-999]
f=col("f"); r=col("r"); l_=col("l"); sh=col("shown")
av=lambda v: (sum(v)/len(v)) if v else -1
er=re.findall(r"earlyReleases=(\d+)",last); rt=re.findall(r"rt ([\d.]+) ms each",last)
print("RESULT n=%s avg=%s max=%s waiting=%s | trace n=%d avgShown=%.2f avgLoadDone=%.2f avgReady=%.2f avgFirstFrame=%.2f | hitches>50=%d >100=%d max=%.0f | fps<50 s=%d of %d | ownedAnimRoots max=%s | earlyRel=%s rtMs=%s | peakGB=%s"%(
 m and m.group(1),m and m.group(2),m and m.group(3),m and m.group(4),len(tr),av(sh),av(l_),av(r),av(f),len(h),sum(1 for x in h if x>100),max(h) if h else 0,sum(1 for x in secs if x<50),len(secs),max(own) if own else "-",er,rt,sys.argv[2] if len(sys.argv)>2 else "-"))
