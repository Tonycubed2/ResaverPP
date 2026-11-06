# Runs the game's own CELL change form code (Fallout4.exe 1.11.240) on every CELL (type 6) body of a dump:
#  0xbf6c40 (initial data type from the change flags, as 0xbf77b0 calls it), the initial data read (0xc0fd90, size from
#  the table 0x2f31860) and 0xbf7090 (parse: detach time at +0xc), then TESObjectCELL::LoadGame (0x4d7b30) on a fake
#  cell from the position after the initial data. Reports bodies read to the exact end and the detach times.
# usage: cellemu.py <dump.bin> [interior ids file: hex ids of cells read as interiors, else from the initial type]
import sys,os
R=os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0,R)
from harness import *
from cf import load
import collections, struct
fv,cfs=load(sys.argv[1]); c=Ctx(fv); e=c.e
want=set(int(x,16) for x in sys.argv[2].split(',')) if len(sys.argv)>2 and sys.argv[2] else None
wsidx=[]
def beac20(e2):
    wsidx.append(e2.r(UC_X86_REG_RDX)); return 0x3c
e.stub(0xbeac20, beac20)
TABLE=0x2f31860
res=collections.Counter(); out=[]
for ref,t,v,fl,fid,b in cfs:
    if t!=6: continue
    if want is not None and fid not in want: continue
    c.reset()
    cell=e.alloc(0x200)
    e.u.mem_write(cell, struct.pack('<Q', BASE+0x24a34d8)); e.u.mem_write(cell+0x1a, bytes([0x3f])); e.u.mem_write(cell+0x14, struct.pack('<I', fid))
    lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8)); e.u.mem_write(cell+0x48, struct.pack('<Q', lst))
    typ=e.call(0xbf6c40, cell, fl) & 0xffffffff
    size=e.u.mem_read(BASE+TABLE+3*typ,1)[0] if typ else 0
    buf=make(e, b, fl, fv)
    detach=None
    if typ:
        raw=e.alloc(64); o=e.alloc(64)
        e.call(0xc0fd90, buf, raw, size, 1)
        e.call(0xbf7090, raw, typ, o)
        detach=struct.unpack('<I', e.u.mem_read(o+0xc,4))[0]
    interior = 1 if typ==3 else 0 if typ in (1,2) else None
    if interior is None:  # bit 31 without bit 30: try the layout that reads to the end (as the C++ decoder does)
        interior=1
    e.u.mem_write(cell+0x40, struct.pack('<H', 1 if interior else 0))
    p0=pos(e,buf)
    try:
        e.call(0x4d7b30, cell, buf)
        p=pos(e,buf)
        ok = p==len(b)
        if not ok and typ==0 and fl & 0x80000000:
            c.reset(); buf=make(e,b,fl,fv); e.u.mem_write(cell+0x40, struct.pack('<H',0)); e.call(0x4d7b30, cell, buf); p=pos(e,buf); ok=p==len(b)
        tag=f'type {typ} ' + ('exact' if ok else f'NOT exact ({p}/{len(b)})')
    except UcError as ex:
        tag=f'type {typ} fault '+hex(e.r(UC_X86_REG_RIP)-BASE)
    res[tag]+=1
    out.append((fid, fl, typ, size, detach, tag))
print(os.path.basename(sys.argv[1]), dict(res))
for x in out:
    if want is not None or 'exact' not in x[5] or 'NOT' in x[5]:
        print('  %08X flags %08X type %d initial %d bytes detach %s %s' % (x[0], x[1], x[2], x[3], 'none' if x[4] is None else '%08X'%x[4], x[5]))
