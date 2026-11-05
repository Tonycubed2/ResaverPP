import sys; sys.path.insert(0,'.')
from pe import *
import struct
FSTART,FEND=0x294760,0x2985f0
CONT=0x29847d
def u8(r): return data[r]
def u32(r): return struct.unpack_from('<I',data,r)[0]
def case_target(t):
    idx=u8(0x298868+t-0xc); off=u32(0x2986e0+idx*4); return off  # rva
def walk(start):
    seen=set(); stack=[start]; ins_all={}
    while stack:
        a=stack.pop()
        while a not in seen and FSTART<=a<FEND:
            seen.add(a)
            ins=next(md.disasm(data[a:a+16], base+a))
            ins_all[a]=ins
            m=ins.mnemonic
            if m=='jmp':
                try: t=int(ins.op_str,16)-base
                except: break
                if t==CONT: break
                a=t; continue
            if m.startswith('j'):
                try: stack.append(int(ins.op_str,16)-base)
                except: pass
            if m=='ret': break
            a+=ins.size
    return [ins_all[k] for k in sorted(ins_all)]
if __name__=='__main__':
    t=int(sys.argv[1],0)
    for i in walk(case_target(t)):
        print(hex(i.address-base), i.mnemonic, i.op_str)
