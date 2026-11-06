# per exe: the extra data type bytes of ExtraActorValueStorage, ExtraLinkedRef and Workshop::ExtraData (their constructors), and the indices the ActorValue singleton gives the four
# workshop budget values (form id = 0x2BC + index). usage: vers.py <exe>
import sys,re,struct,pefile,capstone
import numpy as np
pe=pefile.PE(sys.argv[1],fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase; data=pe.get_memory_mapped_image()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
q=np.frombuffer(data[:len(data)//8*8],dtype=np.uint64); d=np.frombuffer(data[:len(data)//4*4],dtype=np.uint32)
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]; T0=text.VirtualAddress; T1=T0+text.Misc_VirtualSize
def vtable(name):
    m=data.find(name); td=m-0x10; out=[]
    for ci in np.nonzero(d==td)[0]:
        col=int(ci)*4-0x0c
        if col<0 or struct.unpack_from('<I',data,col)[0]!=1 or struct.unpack_from('<I',data,col+4)[0]!=0: continue
        for vi in np.nonzero(q==base+col)[0]: out.append(int(vi)*8+8)
    return out
def riprefs(target):
    # rip-relative leas (7 bytes, the displacement last): int32 views at the 4 alignments, end = position + 4
    out=[]
    for k in range(4):
        n=(T1-T0-k-4)//4
        dd=np.frombuffer(data,dtype=np.int32,count=n,offset=T0+k).astype(np.int64)
        pos=T0+k+4*np.arange(n,dtype=np.int64)
        for i in np.nonzero(pos+4+dd==target)[0]: out.append(int(pos[i])-3)
    return sorted(out)
for cls in (b'.?AVExtraActorValueStorage@@', b'.?AVExtraLinkedRef@@', b'.?AVExtraData@Workshop@@'):
  vt=vtable(cls)
  print(cls.decode(), 'vtable', [hex(x) for x in vt])
  for v in vt:
    for st in riprefs(v):
        for ins in md.disasm(data[st-0x20:st+8],base+st-0x20):
            if 'byte ptr [rcx + 0x12]' in ins.op_str: print('  ctor near', hex(st), ins.mnemonic, ins.op_str)
for nm in (b'WorkshopCurrentTriangles\0', b'WorkshopMaxTriangles\0', b'WorkshopCurrentDraws\0', b'WorkshopMaxDraws\0'):
    s=data.find(b'\0'+nm)+1
    for st in riprefs(s):
        idx=None
        for ins in md.disasm(data[st-0x60:st+0x40],base+st-0x60):
            if ins.mnemonic=='mov' and ins.op_str.startswith('edx, 0x'): idx=int(ins.op_str.split(',')[1],16)
            if ins.mnemonic=='lea' and ins.op_str.startswith('edx, [r9 + 0x'): idx=7+int(ins.op_str.split('+ ')[1].rstrip(']'),16)
            if ins.address-base>st and ins.mnemonic=='call': break
        print(' ', nm.decode().rstrip('\0'), 'lea at', hex(st), 'index', hex(idx) if idx is not None else None, '-> form id', hex(0x2bc+idx) if idx is not None else None)
