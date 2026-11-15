"""The numpy DSP encoder and LPC fit as they were before the C port: the test oracle, test-only."""
import numpy as np

def lpc2(x):
    """Order-2 predictor in DSP fixed point (coef/2048) by least squares."""
    x=np.asarray(x,dtype=np.float64)
    if len(x)<4: return (0,0)
    y=x[2:]; A=np.stack([x[1:-1],x[:-2]],1)
    try: c,*_=np.linalg.lstsq(A,y,rcond=None)
    except Exception: return (0,0)
    c=np.clip(np.round(c*2048),-32768,32767).astype(int); return (int(c[0]),int(c[1]))

def dsp_encode(x, coefs):
    """Brute force each 14-sample frame over 8 predictors x 16 scales (numpy across the 128 lanes)."""
    x=np.asarray(x,dtype=np.int64); n=len(x)
    frames=(n+13)//14
    c1=np.repeat(np.array([c[0] for c in coefs],dtype=np.int64),16)
    c2=np.repeat(np.array([c[1] for c in coefs],dtype=np.int64),16)
    sc=np.tile(np.arange(16,dtype=np.int64),8)
    out=bytearray(); h1=h2=0
    xp=np.concatenate([x,np.zeros(frames*14-n,dtype=np.int64)])
    for f in range(frames):
        blk=xp[f*14:f*14+14]
        H1=np.full(128,h1,dtype=np.int64); H2=np.full(128,h2,dtype=np.int64)
        err=np.zeros(128,dtype=np.int64); nibs=np.zeros((128,14),dtype=np.int64)
        for i in range(14):
            pred=(c1*H1+c2*H2+1024)>>11
            # nibble*2^scale*2048 ... decoder: ((nib<<scale)<<11 + 1024 + c1*h1+c2*h2)>>11
            d=blk[i]-pred
            nib=np.round(d/(1<<sc)).astype(np.int64); nib=np.clip(nib,-8,7)
            v=((nib<<sc)<<11)+1024+c1*H1+c2*H2; v>>=11; v=np.clip(v,-32768,32767)
            e=blk[i]-v; err+=e*e; nibs[:,i]=nib; H2=H1; H1=v
        best=int(np.argmin(err))
        out.append(((best//16)<<4)|(best%16))
        nb=nibs[best]&15
        for i in range(0,14,2): out.append(int(nb[i]<<4|nb[i+1]))
        # recompute history for the chosen lane
        h1,h2=int(H1[best]),int(H2[best])
    return bytes(out), frames
