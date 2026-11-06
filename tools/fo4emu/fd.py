# disassemble a function (follows .pdata chained parts by walking until ret/int3 padding): fd.py <rva hex> [max]
import sys; sys.path.insert(0,'.')
from pe import *
def fd(rva, mx=600):
    out=[]; seen=set(); todo=[rva]
    # linear sweep from rva until a ret followed by int3 padding and no pending jumps beyond
    a=rva; far=rva
    for ins in md.disasm(data[rva:rva+mx*15], base+rva):
        r=ins.address-base
        s=ins.op_str
        if ins.mnemonic.startswith('j') and s.startswith('0x'):
            t=int(s,16)-base
            if rva<=t<rva+0x4000 and t>far: far=t
        out.append(f'{r:x} {ins.mnemonic} {s}')
        if ins.mnemonic in ('ret','jmp') and r>=far:
            nxt=data[r+ins.size]
            if nxt==0xcc: break
        if len(out)>=mx: break
    return out
if __name__=='__main__':
    for l in fd(int(sys.argv[1],16), int(sys.argv[2]) if len(sys.argv)>2 else 600): print(l)
