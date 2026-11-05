import sys;sys.path.insert(0,'.')
from achr import *
def runall(fn, maxn=10**9):
    fv,cfs=load(f'dump/{fn}.bin'); a=A(fv)
    st=collections.Counter(); why=collections.Counter(); t0=time.time(); bad=[]
    k=0
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        k+=1
        if k>maxn: break
        try: p=a.parse(ref,fl,b); err=None
        except (Unsupported,EOFError) as ex: p=None; err='pyrefr'
        except UcError as ex: p=None; err='fault '+hex(a.c.e.r(UC_X86_REG_RIP)-BASE)
        tag='exact' if p==len(b) else err or ('over' if a.overrun else 'short')
        st[tag.split()[0]]+=1
        if tag!='exact':
            why[(tag, hex(a.reads[-1][2]) if a.reads else '')]+=1; bad.append(fid)
    print(fn, dict(st), f'{time.time()-t0:.0f}s', flush=True)
    for k2,v in why.most_common(15): print('  ',v,k2)
    print('  bad', [hex(x) for x in bad[:30]])
if __name__=='__main__':
    runall(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else 10**9)
