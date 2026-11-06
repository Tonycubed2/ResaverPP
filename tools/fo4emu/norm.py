# normalized disassembly of a function in several exes: norm.py <exe>:<rva> ... ; prints whether they match
import pefile,capstone,sys,re
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
cache={}
def load(p):
    if p not in cache:
        pe=pefile.PE(p,fast_load=True); cache[p]=(pe.OPTIONAL_HEADER.ImageBase,pe.get_memory_mapped_image())
    return cache[p]
def fn(p,rva,n=400):
    base,data=load(p); out=[]; far=rva; calls=[]
    for i,ins in enumerate(md.disasm(data[rva:rva+n*15],base+rva)):
        r=ins.address-base; s=ins.op_str
        if ins.mnemonic.startswith('j') and s.startswith('0x'):
            t=int(s,16)-base
            if rva<=t<rva+0x3000 and t>far: far=t
            s='L%+d'%(t-rva) if rva<=t<rva+0x3000 else 'EXT'
        elif ins.mnemonic=='call' and s.startswith('0x'):
            calls.append(int(s,16)-base); s='CALL'
        s=re.sub(r'rip \+ 0x[0-9a-f]+','rip+X',s)
        out.append(f'{ins.mnemonic} {s}')
        if ins.mnemonic in('ret','jmp') and r>=far and data[r+ins.size] in (0xcc,): break
        if i>=n: break
    return out,calls
if __name__=='__main__':
    res=[]
    for a in sys.argv[1:]:
        p,r=a.rsplit(':',1); res.append(fn(p,int(r,16)))
    ref=res[0][0]
    for a,(o,c) in zip(sys.argv[1:],res):
        same=o==ref
        diff=sum(1 for x,y in zip(o,ref) if x!=y)+abs(len(o)-len(ref))
        print(a.split('/')[-2], hex(int(a.rsplit(':',1)[1],16)), len(o),'instructions', 'SAME' if same else f'differs in {diff}', 'calls', [hex(x) for x in c][:12])
