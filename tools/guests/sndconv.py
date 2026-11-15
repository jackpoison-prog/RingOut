"""Xbox mmx (Xbox/IMA ADPCM) -> GameCube mmg (DSP-ADPCM) sound containers."""
import os, struct
STEPS=[7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
IDX=[-1,-1,-1,-1,2,4,6,8]
def ima_decode(data, samples):
    out=[]
    for at in range(0, len(data)-35, 36):
        pred, idx = struct.unpack_from('<hB', data, at); out.append(pred)
        for byte in data[at+4:at+36]:
            for nib in (byte & 15, byte >> 4):
                st=STEPS[idx]; d=st>>3
                if nib&4: d+=st
                if nib&2: d+=st>>1
                if nib&1: d+=st>>2
                pred = pred-d if nib&8 else pred+d
                pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
                idx += IDX[nib&7]; idx = 0 if idx < 0 else 88 if idx > 88 else idx
                out.append(pred)
    return out[:samples]

def lpc2(x):
    """Order-2 predictor in DSP fixed point (coef/2048) by least squares, solved exactly in integers.
    Same answer as numpy.linalg.lstsq (including its minimum-norm answer when the fit is degenerate)."""
    if len(x)<4: return (0,0)
    from fractions import Fraction
    a=x[1:-1]; b=x[:-2]; y=x[2:]
    saa=sum(v*v for v in a); sbb=sum(v*v for v in b); sab=sum(p*q for p,q in zip(a,b))
    sya=sum(p*q for p,q in zip(y,a)); syb=sum(p*q for p,q in zip(y,b))
    det=saa*sbb-sab*sab
    if det:
        c=(Fraction(sya*sbb-syb*sab,det), Fraction(syb*saa-sya*sab,det))
    elif saa+sbb:
        # rank 1: the pseudo-inverse of a rank-1 matrix is its transpose over its squared norm
        c=(Fraction(sya,saa+sbb), Fraction(syb,saa+sbb))
    else:
        return (0,0)
    return tuple(max(-32768,min(32767,round(v*2048))) for v in c)

def coefs_for(x):
    """8 coefficient pairs: whole-signal fit, segment fits, and fixed fallbacks."""
    pairs=[(0,0),(2048,0),(4096,-2048)]
    pairs.append(lpc2(x))
    n=len(x)
    for k in range(4):
        seg=x[k*n//4:(k+1)*n//4]
        if len(seg)>16: pairs.append(lpc2(seg))
    while len(pairs)<8: pairs.append((1024,0))
    return pairs[:8]

class DspEncoder:
    """The DSP-ADPCM search, in C (dspenc.c): compiled once with the system C compiler, then fed every
    clip over a pipe. `cc`: compiler to use; else $CC, else cc, else clang."""

    def __init__(self, cc=None):
        import shutil, subprocess, tempfile
        cands = [cc] if cc else [c for c in (os.environ.get('CC'), 'cc', 'clang', 'gcc') if c]
        comps = list(dict.fromkeys(p for p in map(shutil.which, cands) if p))
        if not comps:
            raise ValueError('no C compiler found to build the sound encoder')
        self.dir = tempfile.mkdtemp(prefix='dspenc-')
        exe = os.path.join(self.dir, 'dspenc')
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dspenc.c')
        # Each in turn: SteamOS's `cc` can be a clang with no C headers while gcc beside it works.
        errors = []
        for comp in comps:
            r = subprocess.run([comp, '-O2', '-std=c99', '-o', exe, src, '-lm'], capture_output=True, text=True)
            if not r.returncode:
                break
            errors.append(comp + ': ' + r.stderr.strip()[-300:])
        else:
            raise ValueError('the sound encoder did not compile:\n' + '\n'.join(errors))
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def encode(self, x, coefs):
        """-> (8-byte frames, frame count), as the numpy encoder returned them."""
        req = struct.pack('<I16h', len(x), *[v for c in coefs for v in c]) + struct.pack('<%dh' % len(x), *x)
        self.p.stdin.write(req); self.p.stdin.flush()
        frames, = struct.unpack('<I', self.p.stdout.read(4))
        out = self.p.stdout.read(frames * 8)
        if len(out) != frames * 8:
            raise ValueError('the sound encoder stopped')
        return out, frames

    def close(self):
        import shutil
        if self.p.poll() is None:
            self.p.stdin.write(struct.pack('<I', 0)); self.p.stdin.close(); self.p.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


_ENC = None


def dsp_encode(x, coefs):
    """Encode one clip with this process's encoder (built on first use)."""
    global _ENC
    if _ENC is None:
        import atexit
        _ENC = DspEncoder(); atexit.register(_ENC.close)
    return _ENC.encode(x, coefs)

def gc_record(a, b, offset, data_size_unused, samples, nibbles, rate, coefs, ps):
    r=bytearray(0xb0)
    struct.pack_into('>I', r, 0, offset)
    pitch=struct.unpack_from('<i', a, 0x1c)[0]; struct.pack_into('>h', r, 4, max(-32768,min(32767,pitch)))
    r[6]=a[2]; r[7]=0x40; r[8]=0x40; r[9]=a[5]
    r[0x14]=0x42; r[0x1e]=0x7c; r[0x1f]=0x06
    r[0x20:0x22]=b'\x3f\x80'
    r[0x2a]=a[0x0b]; r[0x2b]=a[0x0c]; r[0x2c]=1; r[0x2d]=a[0x0e]; r[0x2e]=0x64; r[0x2f]=0x64
    r[0x30:0x32]=b'\x3f\x80'; r[0x34:0x36]=b'\x3f\x80'; r[0x39]=0x7f
    # 0x3c is 0 in every GC character sound record whose bank matches the Xbox's (21 of the 23 shared
    # fighters; Charade and Lizardman are a later GC revision). The Xbox's a[0x0a] (10 in the costume
    # name-call banks) is not it: copying it made his VS-screen name call play at the DSP's maximum rate.
    r[0x3c]=0; r[0x3f]=a[0x34]
    struct.pack_into('>IIIHHIII', r, 0x50, samples, nibbles, rate, 0, 0, 2, nibbles-1, 2)
    struct.pack_into('>16h', r, 0x6c, *[v for c in coefs for v in c])
    struct.pack_into('>HH', r, 0x8c, 0, ps)
    return bytes(r)

def convert_bank(xh, xdata):
    _,_,flags,count,hsz,dsz,oa,sa,ob,sb=struct.unpack_from('<BBHHHIIIII',xh,4)
    recs=[]; data=bytearray(); placed={}
    for i in range(count):
        a=xh[oa+0x58*i:oa+0x58*(i+1)]; b=xh[ob+0x114*i:ob+0x114*(i+1)]
        fmt,ch,rate=struct.unpack_from('<HHI',b,0); off,size=struct.unpack_from('<II',b,0x14)
        key=(off,size)
        if key not in placed:
            samples=size//36*64
            pcm=ima_decode(xdata[off:off+size], samples) if fmt==0x69 else [0]*max(14,samples)
            if not pcm: pcm=[0]*14
            co=coefs_for(pcm); enc,frames=dsp_encode(pcm,co)
            placed[key]=(len(data),len(pcm),2+frames*16-((frames*14-len(pcm)) and 0) ,rate,co,enc[0])
            nib = frames*16  # nibbles incl. headers
            placed[key]=(len(data),len(pcm),len(pcm)+ (len(pcm)+13)//14*2 ,rate,co,enc[0])
            data+=enc; data+=bytes(-len(data)%32)
        o,smp,nib,rt,co,ps=placed[key]
        recs.append(gc_record(a,b,o,0,smp,nib,rate,co,ps))
    hdr=b'CHDp'+struct.pack('>BBHHHI',1,0x10,flags,count,count*0xa0+0x10,len(data))
    return hdr+b''.join(recs), bytes(data)

def convert_container(x):
    assert x[:4]==b'mmx\0'
    nb=struct.unpack_from('<I',x,0x14)[0]
    at=(0x18+16*nb+0x1f)&~0x1f; table=[]; parts=[]
    for k in range(nb):
        ho,hs,do,ds=struct.unpack_from('<4I',x,0x18+16*k)
        if hs==0: h,d=b'',x[do:do+ds]
        else: h,d=convert_bank(x[ho:ho+hs],x[do:do+ds])
        ha=at; at=(at+len(h)+0x1f)&~0x1f; da=at; at=(at+len(d)+0x1f)&~0x1f
        table.append((ha,len(h),da,len(d))); parts.append((ha,h,da,d))
    out=bytearray(at); out[0:4]=b'mmg\0'; struct.pack_into('>I',out,4,at)
    out[8:10]=x[8:10]; out[10:12]=x[10:12][::-1]; out[12:16]=x[12:16]
    struct.pack_into('>II',out,0x10,0x100,nb)
    for k,row in enumerate(table): struct.pack_into('>4I',out,0x18+16*k,*row)
    for ha,h,da,d in parts: out[ha:ha+len(h)]=h; out[da:da+len(d)]=d
    return bytes(out)
