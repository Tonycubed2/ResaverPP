# which Havok object classes (0xed5cf0/0xed5bd0 first appearances) and registry tags (0x89cb10) the failing ACHRs use
# usage: classes.py <dump> <fails list> ; prints class id -> LoadGame (vtable +0x20) and counts, and per-actor end
import sys,collections;sys.path.insert(0,'.')
from achr import *
dump,lst=sys.argv[1],sys.argv[2]
refs=set()
for l in open(lst):
    p=l.split()
    if len(p)>2 and p[0]!='ACHR' and 'TAILEND' not in l: refs.add(int(p[0],16))
fv,cfs=load(f'dump/{dump}.bin'); a=A(fv); e=a.c.e; u=e.u
cls=collections.Counter(); tags=collections.Counter(); where={}
def hk(uu,addr,size,ud):
    rsp=uu.reg_read(UC_X86_REG_RSP); cid=struct.unpack('<I',uu.mem_read(rsp+0x38,4))[0]
    obj=uu.reg_read(UC_X86_REG_RBX); vt=struct.unpack('<Q',uu.mem_read(obj,8))[0]
    fn=struct.unpack('<Q',uu.mem_read(vt+0x20,8))[0]-BASE
    cls[(cid,fn,vt-BASE)]+=1
for ad in (0xed5db7,0xed5c97): e.u.hook_add(UC_HOOK_CODE, hk, begin=BASE+ad, end=BASE+ad)
def tg(uu,addr,size,ud):
    rbx=uu.reg_read(UC_X86_REG_RBX); tags[uu.mem_read(rbx+2,1)[0]]+=1
e.u.hook_add(UC_HOOK_CODE, tg, begin=BASE+0x89cbaa, end=BASE+0x89cbaa)
st=collections.Counter()
for ref,t,v,fl,fid,b in cfs:
    if t!=1 or ref not in refs: continue
    try: p=a.parse(ref,fl,b); r='exact' if p==len(b) else f'end {p} of {len(b)}'
    except Exception as ex: r=f'fault {hex(e.r(UC_X86_REG_RIP)-BASE)} {backtrace(e,5)}'
    st[r.split()[0]]+=1
    print(f'{ref:06x} {r}', flush=True)
print(dump, dict(st))
for (c,f,vt),n in sorted(cls.items()): print(f'class {c:08x} load {f:x} vt {vt:x} n {n}')
print('registry index', dict(tags))
