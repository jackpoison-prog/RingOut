import struct
import os
# Xbox and GC root.olk, from the environment (never committed: they are the user's discs).
X = os.environ.get("RINGOUT_SPAWN_XBOX", "")
G = os.environ.get("RINGOUT_SPAWN_GC", "")
class Olk:
    def __init__(s,src):
        """`src`: a path, or an xiso.FileView into a disc image."""
        s.f=open(src,'rb') if isinstance(src,(str,bytes,os.PathLike)) else src.open(); s.top=s._dir(0)
    def _dir(s,at):
        s.f.seek(at); h=s.f.read(0x20); cnt,mag=struct.unpack('<I4s',h[:8]); hs=struct.unpack('<I',h[0x10:0x14])[0]
        assert mag==b'olnk'
        ents=[struct.unpack('<IIII',s.f.read(16)) for _ in range(cnt)]
        return at+hs,ents
    def group(s,g):
        base,ents=s.top; o,sz,_,_=ents[g]; return s._dir(base+o)
    def entry(s,g,i):
        base,ents=s.group(g); o,sz,_,_=ents[i]
        if not sz: return b''
        s.f.seek(base+o); return s.f.read(sz)
def pack(b,be):
    """numbered pack: u32 count, u32 offsets[count+1]"""
    e='>' if be else '<'
    n=struct.unpack(e+'I',b[:4])[0]
    if n==0 or n>4096 or 4+4*(n+1)>len(b): return None
    offs=struct.unpack(e+'%dI'%(n+1),b[4:8+4*n])
    if any(offs[i]>offs[i+1] for i in range(n)) or offs[-1]>len(b): return None
    return [b[offs[i]:offs[i+1]] for i in range(n)]
