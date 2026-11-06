# runs the game's own havok blob reader (0xc10b00, kind 2, as 0xbf77b0 calls it) and then TESObjectREFR::LoadGame (0x4f5c50)
# on every REFR with change flag 2 of a dump; reports how many read to the exact end and whether a blob was stored
import sys; sys.path.insert(0,'.')
from harness import *
from cf import load
from proto import R, bit, initial_type, initial
import collections, time
fn=sys.argv[1]; lim=int(sys.argv[2]) if len(sys.argv)>2 else 10**9
fv,cfs=load(f'dump/{fn}.bin'); c=Ctx(fv); e=c.e
stored=[0]
def st(e2): stored[0]+=1; return 0
e.stub(0xc10e60, st)
res=collections.Counter(); bad=[]; k=0; t0=time.time()
for ref,t,v,fl,fid,b in cfs:
    if t!=0 or not bit(fl,2): continue
    k+=1
    if k>lim: break
    c.reset(); stored[0]=0
    r=R(b); initial(r, initial_type(ref,fl)); hv=r.p
    rr=R(b); rr.p=hv; rr.u8(); n=rr.vs(); rr.raw(n); hend=rr.p
    try:
        buf=c.newbuf(b, fl, hv, False, False); obj=c.obj
        tbl=e.alloc(0x200)
        e.call(0xc10b00, tbl, 2, buf, 0)
        p1=pos(e,buf); s1=stored[0]
        lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8)); e.u.mem_write(obj+0x100, struct.pack('<Q', lst))
        inv=e.alloc(0x200); e.u.mem_write(obj+0xf8, struct.pack('<Q', inv))
        e.call(0x4f5c50, obj, buf)
        p2=pos(e,buf)
        tag=('blob ok' if p1==hend else 'blob WRONG')+(' stored' if s1 else ' none')+(' size0' if n==0 else '')+(' | exact' if p2==len(b) else ' | NOT exact')
    except UcError as ex:
        tag='fault '+hex(e.r(UC_X86_REG_RIP)-BASE)
    res[tag]+=1
    if 'exact' not in tag or 'WRONG' in tag or 'NOT' in tag: bad.append(hex(fid))
print(fn, dict(res), f'{time.time()-t0:.0f}s'); print(' bad', bad[:20])
