# compare fo4cf::DecodeActor's reads (cpptrace.cpp output) with the game's Actor::LoadGame in the emulator, read by read
# usage: trcmp.py <dump> <cpptrace file> [max actors]
# Reads the decoder does not make are left out on the emulator side only inside ranges it skips as one block: active
# effect data (ActiveEffect::LoadGame, sized), blocks the decoder reads as 'B' (sized blocks: up to its next read).
import sys,struct,collections;sys.path.insert(0,'.')
from achr import *
KIND={'R':'R','R12':'R','XF':'R','REF':'F','VS':'V','WS':'S','WS2':'S','BLOB':'B'}
def loadcpp(p):
    b=open(p,'rb').read(); o=0; out={}
    while o<len(b):
        ref,re_,n=struct.unpack_from('<III',b,o); o+=12; t=[]
        for i in range(n):
            pos=struct.unpack_from('<I',b,o)[0]; k=chr(b[o+4]); o+=5; t.append((pos,k))
        m=struct.unpack_from('<I',b,o)[0]; o+=4; sk=[]
        for i in range(m):
            sk.append(struct.unpack_from('<II',b,o)); o+=8
        out[ref]=(t,sk,re_)
    return out
def compare(mine, skips, emu, prefix=False):
    # regions the decoder reads as one block
    reg=list(skips)
    for i,(p,k) in enumerate(mine):
        if k=='B': reg.append((p, mine[i+1][0] if i+1<len(mine) else 1<<31))
    def inreg(p): return any(a<=p<e for a,e in reg)
    m=[(p,k) for p,k in mine if k!='B']
    e=[(p,KIND[k]) for p,k,_,_ in emu if KIND[k]!='B' and not inreg(p)]
    for i in range(min(len(m),len(e))):
        if m[i]!=e[i]: return f'read {i}: decoder {m[i]} game {e[i]}'
    if prefix: return None if len(e)<=len(m) else f'game made more reads ({len(e)}) than the decoder ({len(m)})'
    if len(m)!=len(e): return f'decoder {len(m)} reads, game {len(e)}'
    return None
if __name__=='__main__':
    dump,cpp=sys.argv[1],sys.argv[2]; mx=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
    want=loadcpp(cpp); fv,cfs=load(f'dump/{dump}.bin'); a=A(fv); st=collections.Counter(); k=0
    import achr
    pyrefr0=achr.pyrefr
    def pyrefr_or_cpp(*x,**kw):
        # the Python REFR decoder (dec.py) where it can; else the C++ decoder's end of the reference part (that part
        # is compared with the game's own TESObjectREFR::LoadGame by refrnative.py)
        try: return pyrefr0(*x,**kw)
        except (Unsupported,EOFError): st['reference end from the decoder']+=1; return cur[0]
    achr.pyrefr=pyrefr_or_cpp; cur=[0]
    for ref,t,v,fl,fid,b in cfs:
        if t!=1 or ref not in want: continue
        k+=1
        if k>mx: break
        cur[0]=want[ref][2]; p=None
        try: p=a.parse(ref,fl,b)
        except Exception as ex:
            # a CPU exception after hundreds of actors comes from state the snapshot does not restore (the same actor
            # reads to its end on a fresh emulator): build a fresh one and read the actor again
            if 'EXCEPTION' in str(ex):
                a=A(fv)
                try: p=a.parse(ref,fl,b); st['fresh emulator']+=1
                except Exception as ex2: ex=ex2; p=None
            if p is None or 'EXCEPTION' not in str(ex):
                # the emulator stopped inside the actor: its reads so far must be the decoder's first reads
                r=compare(want[ref][0], want[ref][1], a.reads, prefix=True)
                if r: st['different']+=1; print(f'{ref:06x} {r} (emulator stopped: {ex})', flush=True)
                else: st['same up to where the emulator stopped']+=1; print(f'{ref:06x} same up to where the emulator stopped ({len(a.reads)} reads): {ex}')
                continue
        if p!=len(b): st['emulator not exact']+=1; print(f'{ref:06x} emulator end {p} of {len(b)}'); continue
        r=compare(want[ref][0], want[ref][1], a.reads)
        if r: st['different']+=1; print(f'{ref:06x} {r}', flush=True)
        else: st['same']+=1
    print(dump, dict(st), flush=True)
