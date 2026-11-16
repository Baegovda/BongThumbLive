"""Split folder-click -> first-frame latency into phases from BTLT tile traces (BongThumbLive.TileTrace 1).

Per tile (seconds after the tile was first seen): q = queued for loading (plugin scheduling / slot wait),
l = package load finished (engine async load), r = animation ready (compressed data / DDC), f = first frame shown.
Usage: python -I btl_trace_split.py <log> [<log> ...]   (prints phase medians/averages for the first screen and the rest)
"""
import re, sys, statistics as st

PAT = re.compile(r'BTLT tile=\S+ shown=([\d.-]+) q=([\d.-]+) l=([\d.-]+) r=([\d.-]+) f=([\d.-]+)')
TS = re.compile(r'^\[(\d+)\.(\d+)\.(\d+)-(\d+)\.(\d+)\.(\d+):(\d+)\]')

def stamp(line):
    m = TS.match(line)
    return int(m.group(4)) * 3600 + int(m.group(5)) * 60 + int(m.group(6)) + int(m.group(7)) / 1000 if m else None

def summary(name, rows):
    if not rows:
        print('  %-12s n=0' % name); return
    cols = {'queue wait (plugin)': [r[1] for r in rows], 'load (engine)': [r[2] - r[1] for r in rows],
            'ready (anim data)': [r[3] - r[2] for r in rows], 'first frame (render)': [r[4] - r[3] for r in rows],
            'total': [r[0] for r in rows]}
    print('  %-12s n=%d  ' % (name, len(rows)) + '  '.join('%s med %.2f avg %.2f' % (k, st.median(v), sum(v) / len(v)) for k, v in cols.items()))

for path in sys.argv[1:]:
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    opened = next((stamp(l) for l in lines if 'BTLD opened' in l), None)
    first, rest = [], []
    for l in lines:
        m = PAT.search(l)
        if not m:
            continue
        v = [float(x) for x in m.groups()]
        if min(v[1:]) < 0:
            continue  # tile played from the disk cache: no load phases
        t = stamp(l)
        shown_at = (t - v[0]) - opened if (t is not None and opened is not None) else 99
        (first if shown_at < 2.0 else rest).append(v)
    print(path)
    summary('first screen', first)
    summary('later', rest)
