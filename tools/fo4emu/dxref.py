# rip-relative data references to an RVA range: python3 dxref.py <lo hex> [hi hex]
import sys,struct; sys.path.insert(0,'.')
from pe import *
import numpy as np
TEXT_END=0x2400000
a=np.frombuffer(data[:TEXT_END+4],dtype=np.uint8)
d=(a[:-3].astype(np.int64)|(a[1:-2].astype(np.int64)<<8)|(a[2:-1].astype(np.int64)<<16)|(a[3:].astype(np.int64)<<24))
d=np.where(d>=2**31,d-2**32,d)
pos=np.arange(len(d))
def refs(lo,hi):
    out=set()
    for k in range(0,5):   # bytes of immediate after the displacement
        t=pos+4+k+d
        for i in np.nonzero((t>=lo)&(t<hi))[0]:
            # confirm by disassembling from a few bytes before
            for back in range(1,8):
                st=int(i)-back
                for ins in md.disasm(data[st:st+16], base+st):
                    if ins.address-base<=i<ins.address-base+ins.size and '[rip' in ins.op_str:
                        tgt=ins.address-base+ins.size+d[i]
                        if lo<=tgt<hi: out.add((ins.address-base, ins.mnemonic+' '+ins.op_str, tgt))
                    break
    return sorted(out)
if __name__=='__main__':
    lo=int(sys.argv[1],16); hi=int(sys.argv[2],16) if len(sys.argv)>2 else lo+8
    for a_,s,t in refs(lo,hi): print(hex(a_), hex(t), s)
