import sys; sys.path.insert(0,'.')
from fsum import *
from cases import case_target
def lin(start, lo, hi, stop, limit=400):
    # execution-order listing: fallthrough first, branches queued after
    out=[]; seen=set(); queue=[start]
    while queue and len(out)<limit:
        a=queue.pop(0)
        if a in seen: continue
        out.append(f'--- {a:x}')
        while lo<=a<hi and a not in stop and len(out)<limit:
            if a in seen: out.append(f'   (-> {a:x})'); break
            seen.add(a)
            ins=next(md.disasm(data[a:a+16], base+a))
            s=f'{a:x} {ins.mnemonic} {ins.op_str}'
            if ins.mnemonic=='call':
                try:
                    t=int(ins.op_str,16)-base; s+='  <'+NAMES.get(t,'')+'>'
                except: pass
            out.append(s)
            m=ins.mnemonic
            if m=='jmp':
                try: a=int(ins.op_str,16)-base; continue
                except: break
            if m.startswith('j'):
                try: queue.append(int(ins.op_str,16)-base)
                except: pass
            if m in('ret','int3'): break
            a+=ins.size
        else:
            if a in stop: out.append(f'   STOP {a:x}')
    return out
if __name__=='__main__':
    for t in sys.argv[1:]:
        t=int(t,0)
        print(f'##### type {t:#x}')
        print('\n'.join(lin(case_target(t),0x294760,0x2985f0,STOP)))
        print()
