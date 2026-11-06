# runs BGSListForm::LoadGame (0x5898f0, vtable 0x24abf80 slot 0x12) on every FLST (type 42) body of a dump;
# also on each body with a null entry put in front, and on that body purified (nulls taken out)
import sys; sys.path.insert(0,'.')
from harness import *
from cf import load
from proto import R, bit
import collections, struct
fn=sys.argv[1]; fv,cfs=load(f'dump/{fn}.bin'); c=Ctx(fv); e=c.e
def run(b, fl):
    c.reset()
    buf=make(e, b, fl, fv)
    obj=e.alloc(0x200); e.u.mem_write(obj, struct.pack('<Q', BASE+0x24abf80)); e.u.mem_write(obj+0x1a, bytes([0x55]))
    e.call(0x5898f0, obj, buf)
    added=struct.unpack('<I', e.u.mem_read(obj+0x40,4))[0]
    return pos(e,buf), added
def parse(b, fl):
    r=R(b)
    if bit(fl,0): r.raw(6)
    pre=b[:r.p]; n=r.u32(); ids=[r.ref() for _ in range(n)]; return pre, ids, b[r.p:]
def build(pre, ids, post):
    return pre+struct.pack('<I',len(ids))+b''.join(bytes([v>>16&255,v>>8&255,v&255]) for v in ids)+post
res=collections.Counter()
for ref,t,v,fl,fid,b in cfs:
    if t!=42 or not bit(fl,31): continue
    pre,ids,post=parse(b,fl)
    p,a=run(b,fl); res['orig '+('exact' if p==len(b) else 'NOT')+(' all entries kept' if a==len(ids) else ' kept %d/%d'%(a,len(ids)))]+=1
    nb=build(pre,[0]+ids+[0],post)
    p,a=run(nb,fl); res['with 2 nulls '+('exact' if p==len(nb) else 'NOT')+(' nulls kept as entries' if a==len(ids)+2 else ' kept %d/%d'%(a,len(ids)+2))]+=1
    pb=build(pre,[x for x in [0]+ids+[0] if x],post)
    p,a=run(pb,fl); res['purified '+('exact, same as original' if p==len(pb) and pb==b else 'DIFF')]+=1
print(fn, dict(res))
