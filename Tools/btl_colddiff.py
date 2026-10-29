import sys, numpy as np
from PIL import Image
D="C:/Users/Revaptor_FX/AppData/Local/Temp/btl_shots/"
for tag in ("before1","after1"):
    out=[]
    for s in range(0,7):
        for a,b in (("01.0","01.3"),("05.0","05.3"),("12.0","12.3")) if s else (("15.0","15.3"),("25.0","25.3")):
            try:
                A=np.asarray(Image.open(D+"cold_%s_s%d_%s.png"%(tag,s,a)).convert("RGB")).astype(int); B=np.asarray(Image.open(D+"cold_%s_s%d_%s.png"%(tag,s,b)).convert("RGB")).astype(int)
            except Exception as e: continue
            m=(np.abs(A-B).max(axis=2)>24); G=40; h,w=m.shape
            c=m[:h//G*G,:w//G*G].reshape(h//G,G,w//G,G).mean(axis=(1,3))>0.02
            lab=np.zeros_like(c,int); n=0
            for y,x in zip(*np.nonzero(c)):
                if lab[y,x]: continue
                n+=1; st=[(y,x)]; lab[y,x]=n
                while st:
                    cy,cx=st.pop()
                    for dy,dx in ((1,0),(-1,0),(0,1),(0,-1)):
                        yy,xx=cy+dy,cx+dx
                        if 0<=yy<c.shape[0] and 0<=xx<c.shape[1] and c[yy,xx] and not lab[yy,xx]: lab[yy,xx]=n; st.append((yy,xx))
            out.append("s%d@%s:%d"%(s,b,n))
            if (s,b) in ((3,"12.3"),(6,"05.3")):
                o=B.copy().astype(np.uint8); o[m]=[255,0,0]; Image.fromarray(o).resize((960,1050)).save(D+"colddiff_%s_s%d_%s.png"%(tag,s,b))
    print(tag," ".join(out))
