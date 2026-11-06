# Independent CELL scanner: python3 -I cells.py <plugin> [hex form id...]
# prints per CELL: raw form id, DATA flags, XCLC x y, worldspace (parent WRLD group label), record flags; summary.
import struct,sys,zlib,collections
d=open(sys.argv[1],'rb').read()
want=set(int(x,16) for x in sys.argv[2:])
out={}
def subs(data):
    p=0;big=0
    while p+6<=len(data):
        st=data[p:p+4];ss=struct.unpack_from('<H',data,p+4)[0]
        if big: ss=big;big=0
        v=data[p+6:p+6+ss]
        if st==b'XXXX': big=struct.unpack_from('<I',v)[0]
        yield st,v; p+=6+ss
def walk(off,end,ws):
    while off<end:
        typ=d[off:off+4]; size=struct.unpack_from('<I',d,off+4)[0]
        if typ==b'GRUP':
            lbl=struct.unpack_from('<I',d,off+8)[0]; gt=struct.unpack_from('<i',d,off+12)[0]
            walk(off+24,off+size,lbl if gt==1 else ws)
            off+=size; continue
        flags,fid=struct.unpack_from('<II',d,off+8)
        if typ==b'CELL':
            data=d[off+24:off+24+size]
            if flags&0x40000: data=zlib.decompress(data[4:])
            df=None; xc=None; ed=b''
            for st,v in subs(data):
                if st==b'DATA': df=v[0] | (v[1]<<8 if len(v)>1 else 0)
                elif st==b'XCLC': xc=struct.unpack_from('<ii',v)
                elif st==b'EDID': ed=v.rstrip(b'\0')
            out[fid]=(df,xc,ws,flags,ed)
        off+=24+size
walk(0,len(d),None)
if want:
    for f in sorted(want): print('%08X'%f, out.get(f))
else:
    c=collections.Counter()
    for f,(df,xc,ws,fl,ed) in out.items():
        c[('interior' if df is not None and df&1 else 'exterior', 'xclc' if xc else 'noxclc', 'ws' if ws else 'nows', 'deleted' if fl&0x20 else '')]+=1
    print(len(out)); [print(k,v) for k,v in sorted(c.items())]
