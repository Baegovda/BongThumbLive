import sys
from PIL import Image
import numpy as np
from scipy import ndimage
def count(p):
    a=np.asarray(Image.open(p).convert('RGB')).astype(int)
    w=(a.min(axis=2)>=200)&((a.max(axis=2)-a.min(axis=2))<12)
    lab,n=ndimage.label(w); objs=ndimage.find_objects(lab); s=ndimage.sum(w,lab,range(1,n+1))
    return [(o[0].start,o[1].start) for i,o in enumerate(objs) if 1900<s[i]<2450 and 88<o[0].stop-o[0].start<108 and 54<o[1].stop-o[1].start<68]
for p in sys.argv[1:]:
    print(p, len(count(p)))
