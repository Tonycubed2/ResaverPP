import sys;sys.path.insert(0,'.')
from ptrace import go
from pdata import func_of
def comp(reads):
    out=[]; prevf=None; cnt=0; start=None
    for p,n,ra,sz in reads:
        f=func_of(ra); f=f[0] if f else ra
        if f!=prevf:
            if prevf is not None: out.append((start,prevf,cnt))
            prevf=f; cnt=0; start=p
        cnt+=1
    out.append((start,prevf,cnt))
    return out
if __name__=='__main__':
    a,b=go(sys.argv[1], int(sys.argv[2],0))
    for s,f,c in comp(a.reads): print(s, hex(f), c)
