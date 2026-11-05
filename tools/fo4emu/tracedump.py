# emulator traces of every ACHR in a dump: trace_<save>.bin
# file: u32 count; per actor: u32 refid, u8 exact, u32 actorStart, u32 end, u32 n, n x (u32 pos, u8 kind, u32 site)
import sys,struct;sys.path.insert(0,'.')
from achr import *
K={'R':ord('R'),'R12':ord('R'),'XF':ord('R'),'REF':ord('F'),'VS':ord('V'),'WS':ord('S'),'WS2':ord('S'),'BLOB':ord('B')}
def dump(fn, maxn=10**9, only=None, out_name=None):
    fv,cfs=load(f'dump/{fn}.bin'); a=A(fv)
    out=[]; st=collections.Counter(); t0=time.time()
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        if only is not None and fid!=only: continue
        if len(out)>=maxn: break
        try: p=a.parse(ref,fl,b); ok=p==len(b)
        except Exception: p=0; ok=False
        st[ok]+=1
        rec=bytearray(struct.pack('<IBIII', ref, 1 if ok else 0, a.refr_end or 0, p or 0, len(a.reads)))
        for pos_,k,ra,sz in a.reads: rec+=struct.pack('<IBI', pos_, K.get(k,ord('?')), ra)
        out.append(bytes(rec))
    with open(out_name or f'trace_{fn}.bin','wb') as f:
        f.write(struct.pack('<I',len(out)))
        for r in out: f.write(r)
    print(fn, dict(st), f'{time.time()-t0:.0f}s')
if __name__=='__main__':
    if len(sys.argv)>2 and sys.argv[2]=='player': dump(sys.argv[1], only=0x14, out_name=f'ptrace_{sys.argv[1]}.bin')
    else: dump(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else 10**9)
