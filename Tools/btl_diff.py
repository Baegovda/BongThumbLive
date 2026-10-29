import sys, numpy as np
from PIL import Image
D="C:/Users/Revaptor_FX/AppData/Local/Temp/btl_shots/"
tag=sys.argv[1]
pairs=[("a0","a1")]+[("%s_%s"%(w,a),"%s_%s"%(w,b)) for w in ("bottom85","top99","bottom113") for a,b in (("01.0","01.3"),("04.0","04.3"),("10.0","10.3"))]
for a,b in pairs:
    A=np.asarray(Image.open(D+"scr_%s_%s.png"%(tag,a)).convert("RGB")).astype(int)
    B=np.asarray(Image.open(D+"scr_%s_%s.png"%(tag,b)).convert("RGB")).astype(int)
    m=(np.abs(A-B).max(axis=2)>24)
    G=40; h,w=m.shape; cells=m[:h//G*G,:w//G*G].reshape(h//G,G,w//G,G).mean(axis=(1,3))>0.02
    # label connected cells
    lab=np.zeros_like(cells,int); n=0
    for y,x in zip(*np.nonzero(cells)):
        if lab[y,x]: continue
        n+=1; st=[(y,x)]; lab[y,x]=n
        while st:
            cy,cx=st.pop()
            for dy,dx in ((1,0),(-1,0),(0,1),(0,-1)):
                yy,xx=cy+dy,cx+dx
                if 0<=yy<cells.shape[0] and 0<=xx<cells.shape[1] and cells[yy,xx] and not lab[yy,xx]:
                    lab[yy,xx]=n; st.append((yy,xx))
    print("%-22s changed px=%7d blobs=%d"%(b,m.sum(),n))
    o=B.copy().astype(np.uint8); o[m]=[255,0,0]
    Image.fromarray(o).resize((960,1050)).save(D+"diff_%s_%s.png"%(tag,b))
