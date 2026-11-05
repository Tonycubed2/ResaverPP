import sys,struct,time;sys.path.insert(0,'.')
from achr import *
# run every CRT static initializer once; report
def run_inits(a, verbose=False):
    e=a.c.e; u=e.u
    lo,hi=0x2441948,0x24660c0
    ok=bad=lim=0; t0=time.time(); badl=[]
    for p in range(lo,hi,8):
        f=struct.unpack_from('<Q',IMG,p)[0]
        if not f: continue
        try:
            e.call(f-BASE, limit=300000)
            if e.r(UC_X86_REG_RIP)!=RET: lim+=1; badl.append(hex(f-BASE))
            else: ok+=1
        except UcError as ex:
            bad+=1; badl.append(hex(f-BASE))
    print('inits ok',ok,'fault',bad,'limit',lim, f'{time.time()-t0:.0f}s')
    return badl
if __name__=='__main__':
    a=A(68)
    b=run_inits(a)
    print(b[:40])
