# run the game's Actor::LoadGame (emulator) on every ACHR of a dumpcf-format file: how many read to the exact end
import sys,os,collections
sys.path.insert(0,'.')
from achr import *
for fn in sys.argv[1:]:
    fv,cfs=load(fn); a=A(fv); st=collections.Counter(); bad=[]
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        try: p=a.parse(ref,fl,b); ok=p==len(b)
        except Exception as ex: ok=False
        st[ok]+=1
        if not ok: bad.append(hex(ref))
    print(os.path.basename(fn), 'exact', st[True], 'not', st[False], bad[:10], flush=True)
