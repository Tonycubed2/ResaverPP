# For an exe: find the ResetInterior command entry, its handler, the set-detach function it calls, and the Papyrus Cell.Reset
# native; print the set-detach function so the detach==0 early return and the exterior flag choice can be read.
# usage: python3 -I verexe.py <Fallout4.exe>
import sys,struct,pefile,capstone
pe=pefile.PE(sys.argv[1],fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.get_memory_mapped_image()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def q(r): return struct.unpack_from('<Q',data,r)[0]
def dis(r,n):
    out=[]
    for i in md.disasm(data[r:r+n*15],base+r):
        out.append(i)
        if len(out)>=n or i.mnemonic in ('int3',): break
    return out
name=data.find(b'\0ResetInterior\0')+1
ptr=struct.pack('<Q',base+name); e=data.find(ptr)
handler=q(e+0x30)-base
print('ResetInterior entry', hex(e), 'opcode', hex(struct.unpack_from('<I',data,e+0x10)[0]), 'handler', hex(handler))
calls=[i for i in dis(handler,40) if i.mnemonic=='call']
for i in dis(handler,30):
    if i.mnemonic in('call','mov') and ('0xffffffff' in i.op_str or i.mnemonic=='call'): print('  ',hex(i.address-base),i.mnemonic,i.op_str)
setd=int(calls[-1].op_str,16)-base
print('set detach fn', hex(setd))
for i in dis(setd,70):
    print('    ',hex(i.address-base),i.mnemonic,i.op_str)
    if i.mnemonic=='ret' and i.address-base>setd+0xe0: break
# Papyrus Cell.Reset: string 'Reset' near 'Cell' registration -> the native's code calls setd with -1
import re
hits=[]
code=data[:pe.sections[0].VirtualAddress+pe.sections[0].Misc_VirtualSize]
for m in re.finditer(rb'\xe8|\xe9',code):
    i=m.start()
    if i+5>len(code): break
    r=struct.unpack_from('<i',code,i+1)[0]
    if i+5+r==setd: hits.append(i)
print('callers of set detach:', [hex(h) for h in hits])
