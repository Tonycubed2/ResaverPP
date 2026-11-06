# run the game's Actor::LoadGame (emulator) on every ACHR of a dumpcf-format file: how many read to the exact end
# REFR_ENDS=<file>: refrcheck.sh's "<refid> end <n> ..." lines from the game's own TESObjectREFR::LoadGame, used as the
# reference part's end where dec.py cannot read it
import sys,os,collections
sys.path.insert(0,'.')
from achr import *
import achr
ends={}
if os.environ.get('REFR_ENDS'):
    for l in open(os.environ['REFR_ENDS']):
        p=l.split()
        if len(p)>2 and p[2]!='None': ends[int(p[0],16)]=int(p[2])
py0=achr.pyrefr; cur=[None]; used=[0]
def py_or_game(*x,**k):
    try: return py0(*x,**k)
    except Exception:
        if cur[0] in ends: used[0]+=1; return ends[cur[0]]
        raise
achr.pyrefr=py_or_game
for fn in sys.argv[1:]:
    fv,cfs=load(fn); a=A(fv); st=collections.Counter(); bad=[]; used[0]=0
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        cur[0]=ref
        try: p=a.parse(ref,fl,b); ok=p==len(b)
        except Exception as ex: ok=False
        if not ok:
            # an emulator that has run many actors can collect state its snapshot does not restore: read the actor
            # again on a fresh one
            a=A(fv)
            try: p=a.parse(ref,fl,b); ok=p==len(b); st['fresh emulator']+=ok
            except Exception: ok=False
        st[ok]+=1
        if not ok: bad.append(hex(ref))
    print(os.path.basename(fn), 'exact', st[True], 'not', st[False], '(fresh emulator %d)'%st['fresh emulator'], bad[:100], 'reference end from the game\'s loader', used[0], flush=True)
