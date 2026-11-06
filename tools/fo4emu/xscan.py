# which extra data handlers (ExtraDataList::LoadGame 0x294760 jump table) reach a vsval read (0xc0fd30) or a loop
import sys,struct; sys.path.insert(0,'.')
from pe import *
STOP={0x294a0c,0x29847d,0x29846f}
READERS={0xc0fd30:'VS',0xc0fd90:'R',0xc0fa10:'REF',0xc0fa90:'WS',0xc0fb90:'WS2',0xc0fdd0:'XF'}
memo={}
def fn_reads(f, depth=0):
    if f in memo: return memo[f]
    memo[f]=set()
    if f in READERS: memo[f]={READERS[f]}; return memo[f]
    if depth>4: return set()
    out=set(); seen=set(); todo=[f]
    while todo:
        a=todo.pop()
        if a in seen: continue
        seen.add(a)
        for i in md.disasm(data[a:a+0x400], base+a):
            r=i.address-base
            if r!=a and r in seen: break
            seen.add(r)
            if i.mnemonic=='call' and i.op_str.startswith('0x'):
                t=int(i.op_str,16)-base; out|=fn_reads(t,depth+1)
            elif i.mnemonic.startswith('j') and i.op_str.startswith('0x'):
                t=int(i.op_str,16)-base
                if i.mnemonic=='jmp' and not (f-0x2000<=t<f+0x4000):
                    out|=fn_reads(t,depth+1); break
                if t<r: out.add('LOOP')
                todo.append(t)
                if i.mnemonic=='jmp': break
            elif i.mnemonic in ('ret','int3'): break
    memo[f]=out; return out
def handler(t):
    idx=data[0x298868+t-0xc]; tgt=struct.unpack_from('<I',data,0x2986e0+idx*4)[0]
    out=set(); seen=set(); todo=[tgt]
    while todo:
        a=todo.pop()
        if a in seen or a in STOP: continue
        seen.add(a)
        for i in md.disasm(data[a:a+0x400], base+a):
            r=i.address-base
            if r in STOP: break
            if i.mnemonic=='call' and i.op_str.startswith('0x'):
                out|=fn_reads(int(i.op_str,16)-base)
            elif i.mnemonic.startswith('j') and i.op_str.startswith('0x'):
                tt=int(i.op_str,16)-base
                if tt<r and tt>=tgt: out.add('LOOP')
                todo.append(tt)
                if i.mnemonic=='jmp': break
            elif i.mnemonic in ('ret',): break
    return tgt,out
if __name__=='__main__':
    for t in [int(x,16) for x in sys.argv[1:]]:
        tgt,o=handler(t); print(hex(t), hex(tgt), sorted(o))
