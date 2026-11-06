# runs the game's own reference-part loaders on the player's (or any ACHR's) reference part in a dumpcf-format file:
# after the actor prefix, form flags / base / scale as the game reads them, then ExtraDataList::LoadGame (0x294760) and
# the inventory loader (0x34ee00) natively, then the animation blob; prints where that ends next to dec.py's end
# (the end achr.py's TESObjectREFR::LoadGame stub uses). Checks edited inventories with the game's own code.
import sys,os
sys.path.insert(0,'.')
from harness import *
from dec import refr as pyrefr, npc as pynpc
from cf import load
for fn in sys.argv[1:]:
    fv,cfs=load(fn); c=Ctx(fv)
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        r=R(b); initial(r, initial_type(ref,fl)); havok(r,fl)
        p=r.p+8  # Actor::LoadGame's u32 u32 before TESObjectREFR::LoadGame
        buf=c.newbuf(b, fl, p, True, ref in (0x14,0x400014)); e=c.e
        if bit(fl,0): p+=6
        if bit(fl,7): p+=3
        if bit(fl,4): p+=4
        e.u.mem_write(buf+0x24, struct.pack('<I',p))
        if fl & EXTRA_MASK_ACT: c.extra(buf)
        inv0=pos(e,buf)
        if fl & 0x8000020: c.inventory(buf)
        p=pos(e,buf); inv1=p
        if bit(fl,28):
            rr=R(b); rr.p=p; rr.u8(); n=rr.vs(); rr.raw(n); p=rr.p
        py=pyrefr(b, ref, fl, fv, achr=True, npc_fn=lambda r_,f_: pynpc(r_,f_,fv))
        print(os.path.basename(fn), hex(fid), 'native reference part end', p, 'dec.py', py, 'inventory', inv0, inv1, 'SAME' if p==py else 'DIFFERENT', flush=True)
        c.reset()
