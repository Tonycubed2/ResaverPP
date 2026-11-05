import pefile, struct, capstone
pe=pefile.PE('Fallout4.exe', fast_load=True)
base=pe.OPTIONAL_HEADER.ImageBase
data=pe.get_memory_mapped_image()
def q(rva): return struct.unpack_from('<Q',data,rva)[0]
def vfunc(vt, idx): return q(vt+8*idx)-base
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail=False
def dis(rva, n=400, stop_ret=True):
    out=[]
    for ins in md.disasm(data[rva:rva+n*15], base+rva):
        out.append(ins)
        if len(out)>=n: break
        if stop_ret and ins.mnemonic in('ret','int3') and len(out)>1: break
    return out
def func_end(rva):
    # .pdata lookup
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    best=None
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        if e.struct.BeginAddress<=rva<e.struct.EndAddress: return e.struct.BeginAddress, e.struct.EndAddress
    return None
def disrange(a,b):
    return list(md.disasm(data[a:b], base+a))
