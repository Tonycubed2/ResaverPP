# emulate one ACHR (by refid) with exact call stacks; print reads from a position on
import sys,os,struct
sys.path.insert(0,'.')

from pdata import _s as PB  # function starts (.pdata)
from achr import *
FB=set(PB)
def run(dump, refs, start=0, end=10**9):
    fv,cfs=load(f'dump/{dump}.bin'); a=A(fv); e=a.c.e; recs=[]; sh=[]; on=[False]
    if NATIVE:  # the game's own TESObjectREFR::LoadGame (0x4f5c50) instead of the Python REFR decoder's end
        del e.stubs[BASE+0x4f5c50]
        import achr; achr.pyrefr=lambda *x,**k: 0
    def blk(u,addr,size,ud):
        if not on[0]: return
        rsp=u.reg_read(UC_X86_REG_RSP)
        while sh and sh[-1][0]<rsp: sh.pop()
        r=addr-BASE
        if r in FB:
            ret=struct.unpack('<Q',u.mem_read(rsp,8))[0]-BASE
            while sh and sh[-1][0]<=rsp: sh.pop()
            sh.append((rsp,r,ret))
    e.u.hook_add(UC_HOOK_BLOCK, blk, begin=BASE, end=BASE+0x3000000)
    for addr,n in RDF.items():
        def h(u,ad,sz,ud,n=n):
            e2=a.c.e; buf=e2.r(UC_X86_REG_RCX)
            recs.append((pos(e2,buf), n, e2.r(UC_X86_REG_R8)&0xffffffff, [(f,rt) for _,f,rt in sh]))
        e.u.hook_add(UC_HOOK_CODE, h, begin=BASE+addr, end=BASE+addr)
    for ref,t,v,fl,fid,b in cfs:
        if t!=1 or ref not in refs: continue
        recs.clear(); sh.clear(); on[0]=True
        try: p=a.parse(ref,fl,b); err=''
        except Exception as ex: p=-1; err=f'{ex} rip {e.r(UC_X86_REG_RIP)-BASE:x} stack '+' '.join(f'{f:x}@{rt:x}' for _,f,rt in sh[-12:])
        on[0]=False
        print(f'== {ref:x} fid {fid:x} end {p} of {len(b)} {err}')
        for p_,k,sz,fr in recs:
            if start<=p_<end:
                fs=[f for f,rt in fr]
                j=max([q for q,f in enumerate(fs) if f in (0xc8abd0,0xd65bd0)]+[0]) if not NATIVE else max([q for q,f in enumerate(fs) if f in (0x4f5c50,)]+[0])
                val=b[p_:p_+min(sz,8)].hex() if k=='R' else b[p_:p_+3].hex() if k=='REF' else ''
                print(f'{p_:6d} {k:4s} {sz:3d} {val:16s} '+' '.join(f'{f:x}@{rt:x}' for f,rt in fr[j+1:]))
NATIVE=os.environ.get('NATIVE')=='1'
if __name__=='__main__':
    run(sys.argv[1], {int(sys.argv[2],16)}, int(sys.argv[3]) if len(sys.argv)>3 else 0, int(sys.argv[4]) if len(sys.argv)>4 else 10**9)
