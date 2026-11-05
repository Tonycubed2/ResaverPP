import sys; sys.path.insert(0,'.')
from pe import *
from fsum import walk
N={0xc0fd90:'READ',0xc0fa10:'REFID',0xc0fd30:'VSVAL',0xc0fa90:'WSTR',0xc0fb90:'WSTR2',0x311b80:'lookup',0x22c46ae:'dyncast',0x4f5c50:'REFR::LoadGame',0x1657de0:'heapalloc',0x16583b0:'heapinit',0x1e0320:'alloc',0x1658240:'free',0x1659890:'arrinit',0xc0fdd0:'READXF',0x1659480:'lock',0x16596e0:'unlock'}
def fw(a, out=sys.stdout, span=0x6000):
    for i in walk(a, a-0x100, a+span, set()):
        if i.mnemonic in ('nop','int3'): continue
        s=f'{i.address-base:x} {i.mnemonic} {i.op_str}'
        if i.mnemonic=='call':
            try: t=int(i.op_str,16)-base; s+='  <'+N.get(t,hex(t))+'>'
            except: pass
        print(s,file=out)
if __name__=='__main__':
    fw(int(sys.argv[1],0))
