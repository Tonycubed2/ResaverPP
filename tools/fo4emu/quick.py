import sys;sys.path.insert(0,'.')
from achr import *
def quick(fn,n=25,skip=0):
    fv,cfs=load(f'dump/{fn}.bin'); a=A(fv)
    if a.initerr: print('initerr',a.initerr)
    k=0; st=collections.Counter()
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        k+=1
        if k<=skip: continue
        if k>skip+n: break
        try: p=a.parse(ref,fl,b); res=(p,len(b))
        except Exception as ex: res=repr(ex)[:40]; p=None
        tag='exact' if p==len(b) else 'over' if a.overrun else 'other'
        st[tag]+=1
        if tag!='exact' or n<=30: print(hex(fid), res, len(a.reads), tag, a.reads[-1][2:3] and hex(a.reads[-1][2]))
    print(dict(st))
if __name__=='__main__': quick(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else 25, int(sys.argv[3]) if len(sys.argv)>3 else 0)
