# the game's own procedure / package data factory (singleton 0x32174c0, entries at +0x20, 256 slots) after the CRT
# initializers: names come from the fixed string globals the initializers set (lea rdx,[literal]; lea rcx,[global];
# call 0x167c1e0), matched by the key value each registration stored
import sys,pickle,re;sys.path.insert(0,'.')
from achr import *
import capstone
pe_=IMG
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
xs=open('/tmp/claude-0/-home-claude/2c0005dd-b415-560f-be25-db472ea63176/scratchpad/re/Fallout4.exe','rb')
names={}
code=bytes(IMG[:0x2400000])
for m in re.finditer(rb'\xe8', code):
    i=m.start()
    t=i+5+struct.unpack_from('<i',code,i+1)[0]
    if t!=0x167c1e0: continue
    ins=list(md.disasm(code[i-14:i+5], BASE+i-14))
    if len(ins)<3: continue
    lit=glob=None
    for x in ins:
        mm=re.search(r'\[rip \+ (0x[0-9a-f]+)\]',x.op_str)
        if x.mnemonic=='lea' and mm:
            tgt=x.address-BASE+x.size+int(mm.group(1),16)
            if x.op_str.startswith('rdx'): lit=tgt
            if x.op_str.startswith('rcx'): glob=tgt
    if lit and glob and lit<len(IMG):
        s=bytes(IMG[lit:lit+64]).split(b'\0')[0]
        try: names[glob]=s.decode()
        except: pass
a=A(65, fakes=False); e=a.c.e; u=e.u
def q(x): return struct.unpack('<Q',u.mem_read(x,8))[0]
byval={}
for g,n in names.items():
    v=q(BASE+g)
    if v: byval.setdefault(v,n)
G=BASE+0x32174c0; tbl=q(G+0x1820); cap=struct.unpack('<I',u.mem_read(G+0xc,4))[0]
fm=pickle.load(open('factory.pkl','rb')); real={}
for i in range(cap):
    k,c,n=struct.unpack('<QQQ',u.mem_read(tbl+24*i,24))
    if not n: continue
    real[byval.get(k,hex(k))]=(q(q(c)+8)-BASE, c)
for n in sorted(real): print(f'{n:20s} create {real[n][0]:x}', '' if n in fm else '  <- not in factory.pkl')
print('in factory.pkl, not registered:', [n for n in fm if n not in real])
pickle.dump({n:v[0] for n,v in real.items()}, open('realfactory.pkl','wb'))
