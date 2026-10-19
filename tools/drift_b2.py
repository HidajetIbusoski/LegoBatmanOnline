# Zaehlt je Frame zweier Speicherabzuege (LBO_MEMDUMP) echte Gleitkomma-Abweichungen: relativ 1e-6 bis 1e-2.
# Aufruf: python tools/drift_b2.py runs/batman2/xa runs/batman2/xb
import numpy as np, os, sys, struct
BASE=0x0101F000; per=0x38E7EC; SIZE=per//4*4
def fr(run):
    n=os.path.getsize(run+'/memdump.bin')//per
    raw=np.fromfile(run+'/memdump.bin',dtype=np.uint8)[:n*per].reshape(n,per)
    return raw[:,:SIZE].copy().view(np.uint32)
a=fr(sys.argv[1]); b=fr(sys.argv[2])
for fi in range(len(a)):
    x=a[fi].view(np.float32); y=b[fi].view(np.float32)
    ok=np.isfinite(x)&np.isfinite(y)&(np.abs(x)>1e-6)&(np.abs(x)<1e6)&(a[fi]!=b[fi])
    rel=np.zeros_like(x); rel[ok]=np.abs(x[ok]-y[ok])/np.abs(x[ok])
    m=ok&(rel<1e-2)&(rel>1e-6)
    idx=np.nonzero(m)[0]
    print('Frame-Index',fi,'Drift-Werte',len(idx),' '.join(hex(BASE+4*k) for k in idx[:12]))
