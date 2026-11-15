"""PS2 model parser. Part 0 of a costume or weapon: batches of chunks, each a material and VIF packets of vertices.

Batch table at 0x28 (count at byte 8). Type 1 is skinned: its u8 palette maps a packet's joint indices. Type 0 is
rigid: palette[0] is the joint it hangs from. Each chunk is 0x30 or 0x50 bytes plus a 0x20 tail with the texture and
either a direct packet (tail[1] & 0xff == 2, quadwords in >> 16) or a DMA list of them. cnt 9 marks a second pass."""
import struct


class Model:
    def __init__(self, P):
        self.P = P; self.nb = P[8]; self.njoints = P[10]; self.batches = []
        for i in range(self.nb):
            t = self.u(0x28 + i * 4); ty, nl, fl, pal = P[t], P[t + 1], P[t + 2], P[t + 3]
            if ty == 1:
                palette = list(P[t + 4:t + 4 + pal]); a = (t + 4 + pal + 3) & ~3
            else:
                palette = [pal]; a = t + 4
            while self.u(a) == 0: a += 4   # zero padding up to the list pointers
            lists = [self.u(a + k * 4) for k in range(nl)]
            self.batches.append(dict(type=ty, flag=fl, palette=palette, at=t, lists=[(self.u(l), self.u(l + 4)) for l in lists]))

    def u(self, a): return struct.unpack_from('<I', self.P, a)[0]

    def chunks(self, addr, limit):
        P = self.P; out = []; a = addr
        while a < limit and P[a] == 7:
            sz, cnt = P[a + 1], P[a + 2]
            tail = struct.unpack_from('<3I', P, a + sz + 0x14)
            out.append(dict(at=a, sz=sz, cnt=cnt, floats=struct.unpack_from('<%df' % ((sz - 16) // 4), P, a + 16), tex=self.u(a + sz + 4), tail=tail))
            a += sz + 0x20
        return out

    def dma(self, a):
        out = []
        while True:
            t = self.u(a); q, i = t & 0xffff, t >> 28
            if i == 3:
                out.append((self.u(a + 4), q)); a += 16
            elif i == 1:
                a += 16 + q * 16
            elif i == 6:
                return out
            else:
                raise ValueError('DMA tag 0x%08X at 0x%X' % (t, a))

    def packets_of(self, c):
        t = c['tail']
        return [(t[2], t[1] >> 16)] if t[1] & 0xff == 2 else self.dma(t[2])

    def vif(self, o, q):
        """A VIF packet's UNPACKs: dicts of addr, vn, vl, num, at."""
        P = self.P; a = o; end = o + q * 16; out = []
        while a < end:
            w = self.u(a); imm, num, c = w & 0xffff, (w >> 16) & 0xff, (w >> 24) & 0x7f
            if c >= 0x60:
                vn, vl, n = ((c >> 2) & 3) + 1, (32, 16, 8, 5)[c & 3], num or 256
                out.append(dict(addr=imm & 0x3ff, vn=vn, vl=vl, num=n, at=a + 4))
                a += 4 + ((vl * vn * n + 31) // 32) * 4
            elif c in (0, 1, 3, 4, 5, 6, 7, 0x10, 0x11, 0x13, 0x14, 0x15, 0x17): a += 4
            elif c == 0x20: a += 8
            elif c in (0x30, 0x31): a += 20
            else: raise ValueError('VIF code 0x%08X at 0x%X' % (w, a))
        return out

    def skinned_verts(self, ups):
        """Vertices of a skinned packet: per influence two quads (x y z weight)(nx ny nz, joint<<2 | 0x8000 on the last),
        then (u v 1 flag; flag 1 = no kick), then a colour of 4 floats."""
        P = self.P; a = ups[0]['at']; nloop = self.u(a) & 0x7fff; a += 16; vs = []
        for _ in range(nloop):
            infl = []
            while True:
                x, y, z, wt, nx, ny, nz = struct.unpack_from('<7f', P, a); j = self.u(a + 28); a += 32
                infl.append((x, y, z, wt, nx, ny, nz, (j & 0x7fff) >> 2))
                if j & 0x8000: break
            uu, vv = struct.unpack_from('<2f', P, a); fl = self.u(a + 12); a += 16
            vs.append(dict(infl=infl, uv=(uu, vv), kick=fl, col=struct.unpack_from('<4f', P, a))); a += 16
        return vs

    def rigid_verts(self, ups):
        """Vertices of a rigid packet: positions at VU address 3 (with the flag), normals 4, colours 2, UVs 1."""
        P = self.P; by = {u['addr']: u for u in ups if u['num'] > 1}; vs = []
        for i in range(by[3]['num']):
            x, y, z, f = struct.unpack_from('<3fI', P, by[3]['at'] + 16 * i)
            vs.append(dict(pos=(x, y, z), nrm=struct.unpack_from('<3f', P, by[4]['at'] + 12 * i),
                           col=struct.unpack_from('<f', P, by[2]['at'] + 4 * by[2]['vn'] * i)[0],
                           uv=struct.unpack_from('<2f', P, by[1]['at'] + 8 * i), kick=f))
        return vs
