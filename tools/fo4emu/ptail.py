import sys;sys.path.insert(0,'.')
from achr import *
from comp import comp
import collections
def go(fn, fid=0x14, tail=40, after=None):
    fv,cfs=load(f'dump/{fn}.bin'); a=A(fv)
    blocks=collections.deque(maxlen=30)
    a.c.e.u.hook_add(UC_HOOK_BLOCK, lambda u,addr,size,ud: blocks.append(addr-BASE))
    for ref,t,v,fl,fid2,b in cfs:
        if t==1 and fid2==fid: break
    err=None
    try: p=a.parse(ref,fl,b)
    except Exception as ex: err=repr(ex); p=None
    print(fn,'end',p,len(b),'overrun' if a.overrun else '',err, hex(a.c.e.r(UC_X86_REG_RIP)-BASE) if err else '', [hex(x) for x in blocks][-8:] if err else '')
    if after is not None:
        for x in a.reads:
            if x[0]>=after: print(x[0],x[1],hex(x[2]),x[3] if x[1]=='R' else '', b[x[0]:x[0]+8].hex(' '))
    else:
        for s_,f,c in comp(a.reads)[-tail:]: print(s_, hex(f), c)
    return a,b
if __name__=='__main__':
    go(sys.argv[1], int(sys.argv[2],0), int(sys.argv[3]) if len(sys.argv)>3 else 40, int(sys.argv[4]) if len(sys.argv)>4 else None)
