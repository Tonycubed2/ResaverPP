import sys; sys.path.insert(0,'.')
from pe import *
import struct
STOP={0x2948c0,0x29847d,0x29846f,0x298471}
NAMES={0xc0fd90:'READ',0xc0fa10:'REFID',0xc0fd30:'VSVAL',0x1e0320:'alloc',0x2a0350:'getExisting',0x29f910:'addExtra',0x5194c0:'getHandle',0x311b80:'lookupForm',0x22c46ae:'dyncast',0x1659480:'lock',0x16596e0:'unlock',0x165b9d0:'?'}
def walk(start, lo=0, hi=1<<40, stop=STOP):
    seen=set(); stack=[start]; ins_all={}
    while stack:
        a=stack.pop()
        while a not in seen and lo<=a<hi and a not in stop:
            seen.add(a)
            try: ins=next(md.disasm(data[a:a+16], base+a))
            except StopIteration: break
            ins_all[a]=ins
            m=ins.mnemonic
            if m=='jmp':
                try: t=int(ins.op_str,16)-base
                except: break
                a=t; continue
            if m.startswith('j'):
                try: stack.append(int(ins.op_str,16)-base)
                except: pass
            if m in('ret','int3'): break
            a+=ins.size
    return [ins_all[k] for k in sorted(ins_all)]
def summary(fn, depth=0, maxd=2, seenf=None, lo=0, hi=1<<40, stop=set()):
    seenf=seenf if seenf is not None else set()
    out=[]
    ins=walk(fn,lo,hi,stop)
    last={}
    for i in ins:
        if i.mnemonic in('mov','lea') and i.op_str.startswith(('r8d,','r9d,','r8,')):
            last[i.op_str.split(',')[0]]=i.op_str.split(',',1)[1].strip()
        if i.mnemonic=='call':
            try: t=int(i.op_str,16)-base
            except:
                out.append('  '*depth+f'{i.address-base:x} VCALL {i.op_str}'); continue
            n=NAMES.get(t)
            if n=='READ': out.append('  '*depth+f'{i.address-base:x} READ r8={last.get("r8d","?")} r9={last.get("r9d","?")}')
            elif n in('REFID','VSVAL'): out.append('  '*depth+f'{i.address-base:x} {n}')
            elif n: pass
            else:
                out.append('  '*depth+f'{i.address-base:x} call {t:#x}')
                if depth<maxd and t not in seenf:
                    seenf.add(t)
                    sub=summary(t,depth+1,maxd,seenf)
                    if any(('READ' in s or 'REFID' in s or 'VSVAL' in s or 'VCALL' in s) for s in sub): out+=sub
                    else: out.pop()
        if i.mnemonic.startswith('cmp') and ('+ 0x' in i.op_str) and ('r15' in i.op_str or 'rcx' in i.op_str or 'rbx' in i.op_str or 'rdi' in i.op_str):
            pass
    return out
