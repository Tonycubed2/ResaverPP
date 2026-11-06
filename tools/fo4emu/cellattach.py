# The cell attach reset check in an exe: callers of the get-detach-time function whose code then tests the result for 0 and
# compares it with -1 (reset). usage: python3 -I verattach.py <exe> <get-detach rva hex>
import sys,struct,re,pefile,capstone
pe=pefile.PE(sys.argv[1],fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.get_memory_mapped_image()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
g=int(sys.argv[2],16)
t=pe.sections[0]; code=data[:t.VirtualAddress+t.Misc_VirtualSize]
for m in re.finditer(rb'\xe8',code):
    i=m.start()
    if i+5>len(code) or i+5+struct.unpack_from('<i',code,i+1)[0]!=g: continue
    ins=list(md.disasm(data[i:i+0x80],base+i))[:24]
    txt=[(hex(x.address-base),x.mnemonic,x.op_str) for x in ins]
    if any(x[1]=='cmp' and x[2].endswith(', -1') for x in txt) and any(x[1]=='test' for x in txt[:4]):
        for x in txt: print(*x)
        print('--')
