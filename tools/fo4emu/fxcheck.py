# fxcheck.py <original dump> <edited dump>: the game's Actor::LoadGame / PlayerCharacter::LoadGame (emulator, achr.py) on
# every actor fxremove.cpp edited. An actor passes when its edited body reads to the exact end; where the emulator stops
# inside an actor (its limits, see README), the original body is read too and the actor passes when both stop at the same
# distance from the body's end and every read of the edited body is a read of the original one (the same reads up to the
# effect list and the same reads, aligned from the body's end, after the removed entries).
import sys,os,collections
sys.path.insert(0,'.')
from achr import *
fvo,orig=load(sys.argv[1]); fve,edit=load(sys.argv[2])
oby={x[0]:x for x in orig}
a=A(fve); st=collections.Counter(); bad=[]
def run(ref,fl,b):
    global a
    for fresh in (False,True):
        if fresh: a=A(fve)
        try:
            p=a.parse(ref,fl,b)
            if p==len(b): return True,[(q,n) for q,n,*_ in a.reads]
        except Exception: pass
    return False,[(q,n) for q,n,*_ in a.reads]
for ref,t,v,fl,fid,b in edit:
    if t!=1: continue
    ok,re_=run(ref,fl,b)
    if ok: st['exact']+=1; continue
    o=oby[ref]; ok2,ro=run(ref,o[3],o[5])
    lo,le=len(o[5]),len(b)
    h=0
    while h<min(len(ro),len(re_)) and ro[h]==re_[h]: h+=1
    to=[(lo-p,n) for p,n in ro]; te=[(le-p,n) for p,n in re_]
    k=0
    while k<min(len(to),len(te)) and to[-1-k]==te[-1-k]: k+=1
    if not ok2 and re_ and ro and lo-ro[-1][0]==le-re_[-1][0] and h+k>=len(re_): st['same stop as the original']+=1
    else: st['BAD']+=1; bad.append(hex(ref))
print(os.path.basename(sys.argv[2]), dict(st), bad[:50], flush=True)
