# dump the script/console command tables of any Fallout4.exe: cmdtab_any.py <exe> <out>
import pefile, struct, sys
pe=pefile.PE(sys.argv[1], fast_load=True)
base=pe.OPTIONAL_HEADER.ImageBase
data=pe.get_memory_mapped_image()
def q(o): return struct.unpack_from('<Q',data,o)[0]
def u32(o): return struct.unpack_from('<I',data,o)[0]
def cstr(va):
    o=va-base
    if o<=0 or o>=len(data): return ''
    e=data.find(b'\0',o)
    return data[o:e].decode('latin1')
S=0x50
def anchor_for(name):
    s=data.find(b'\0'+name.encode()+b'\0')+1
    va=struct.pack('<Q',base+s)
    o=0
    while True:
        o=data.find(va,o)
        if o<0: return None
        if o%8==0: return o
        o+=1
def dump(anchor, f):
    o=anchor
    while u32(o-S+0x10)==u32(o+0x10)-1: o-=S
    start=o
    o=anchor
    while u32(o+S+0x10)==u32(o+0x10)+1: o+=S
    end=o+S
    f.write(f"# table {start:#x} {end:#x} {u32(start+0x10):#x} {u32(end-S+0x10):#x}\n")
    for o in range(start,end,S):
        pc=struct.unpack_from('<H',data,o+0x22)[0]; pp=q(o+0x28); params=[]
        if pp and pc<20:
            for k in range(pc):
                po=pp-base+k*0x10
                params.append(f"{cstr(q(po))}:{u32(po+8):#x}{'?' if data[po+12] else ''}")
        ex=q(o+0x30)
        f.write(f"{u32(o+0x10):#06x}\t{cstr(q(o))}\t{cstr(q(o+8))}\tref={data[o+0x20]}\texec={(ex-base) if ex else 0:#x}\t{' '.join(params)}\t| {cstr(q(o+0x18))}\n")
with open(sys.argv[2],'w') as f:
    dump(anchor_for('GetWantBlocking'),f); dump(anchor_for('SaveGame'),f)
