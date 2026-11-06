# the game's own TESObjectREFR::LoadGame (0x4f5c50) on ACHR bodies (as Actor::LoadGame calls it): the positions of the
# extra data type bytes it reads (0x2948dc) and where it ends; compare with refrpos.cpp. usage: refrnative.py <dump> [refid hex...]
import sys,os;sys.path.insert(0,'.')
from achr import *
import achr
dump=sys.argv[1]; refs={int(x,16) for x in sys.argv[2:]}
# a dump name (dump/<name>.bin) or a dumpcf-format file; no refids: every ACHR in it
fv,cfs=load(dump if dump.endswith('.bin') else f'dump/{dump}.bin')
if not refs: refs={x[0] for x in cfs if x[1]==1}; a=A(fv); e=a.c.e
del e.stubs[BASE+0x4f5c50]; achr.pyrefr=lambda *x,**k: 0
types=[]; end=[None]
def tb(u,addr,size,ud): types.append(pos(a.c.e, a.c.e.r(UC_X86_REG_RCX)))
e.u.hook_add(UC_HOOK_CODE, lambda u,ad,sz,ud: types.append(pos(a.c.e, a.buf)) if False else None, begin=BASE+0x2948dc, end=BASE+0x2948dc)
# the type byte is read by c0fd90 returning to 0x2948dc: record the reader's position at its entry when the return is there
def rd(u,addr,size,ud):
    rsp=u.reg_read(UC_X86_REG_RSP); ra=struct.unpack('<Q',u.mem_read(rsp,8))[0]-BASE
    if ra==0x2948dc: types.append(pos(a.c.e, a.c.e.r(UC_X86_REG_RCX)))
e.u.hook_add(UC_HOOK_CODE, rd, begin=BASE+0xc0fd90, end=BASE+0xc0fd90)
def done(u,addr,size,ud): end[0]=pos(a.c.e, a.buf); u.emu_stop()
e.u.hook_add(UC_HOOK_CODE, done, begin=BASE+0xc8acf1, end=BASE+0xc8acf1)
for ref,t,v,fl,fid,b in cfs:
    if t!=1 or ref not in refs: continue
    types.clear(); end[0]=None
    try: a.parse(ref,fl,b)
    except Exception: pass
    print(f'{ref:06X} end {end[0]} types', ' '.join(map(str,sorted(types))))
