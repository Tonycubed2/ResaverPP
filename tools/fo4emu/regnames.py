# every registration into the procedure / package data factory (0x705b60 on 0x706320's or 0x7106f0's singleton):
# the key's fixed string global, its literal, the creator object and its create function
import sys,re,struct; sys.path.insert(0,'.')
from pe import *
code=data[:0x2400000]
def rip(x):
    mm=re.search(r'\[rip \+ (0x[0-9a-f]+)\]',x.op_str)
    return x.address-base+x.size+int(mm.group(1),16) if mm else None
# fixed string globals: (lea rdx,[lit] ; lea rcx,[glob] ; call 167c1e0) or (lea rdx,[lit]; ...167c1e0 tmp; lea rcx,[glob]; call 167c470)
lit_of={}
for m in re.finditer(rb'\xe8', code):
    i=m.start(); t=i+5+struct.unpack_from('<i',code,i+1)[0]
    if t not in (0x167c1e0,0x167c470): continue
    ins=list(md.disasm(code[i-40:i+5], base+i-40))
    lit=glob=None
    for x in ins:
        r=rip(x)
        if x.mnemonic=='lea' and r is not None:
            if x.op_str.startswith('rdx') and r>0x2400000: lit=r
            if x.op_str.startswith('rcx'): glob=r
    if lit and glob:
        s=data[lit:lit+48].split(b'\0')[0]
        if s and all(32<=c<127 for c in s): lit_of.setdefault(glob,s.decode())
out=[]
for m in re.finditer(rb'\xe8', code):
    i=m.start(); t=i+5+struct.unpack_from('<i',code,i+1)[0]
    if t!=0x705b60: continue
    ins=list(md.disasm(code[i-60:i+5], base+i-60))
    key=cre=None
    for x in ins:
        r=rip(x)
        if x.mnemonic=='lea' and r is not None:
            if x.op_str.startswith('rdx'): key=r
            if x.op_str.startswith('rax'): cre=r
    out.append((i,key,cre))
for i,key,cre in out:
    # the creator object's vtable is stored by the code right after the call (lea rax,[vt]; mov [rip+cre],rax)
    vt=None
    for x in md.disasm(code[i+5:i+40], base+i+5):
        r=rip(x)
        if x.mnemonic=='lea' and x.op_str.startswith('rax') and r: vt=r; break
    create=q(vt+8)-base if vt else None
    print(hex(i), 'key', hex(key) if key else None, lit_of.get(key,'?'), 'creator', hex(cre) if cre else None, 'create', hex(create) if create else None)
