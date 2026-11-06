# the game's handling of extra 0x6c (0x295f2b: 0xceaed0(type), LoadGame vtable +0x30 with {buffer, 0}) on each entry the
# C++ decoder found; the end must be the decoder's entry end. args: dump name, file "refid begin end"
import sys,os,struct
sys.path.insert(0,'.')
from achr import *
rows={}
for l in open(sys.argv[2]):
    r,b_,e_=l.split(); rows.setdefault(int(r,16),[]).append((int(b_),int(e_)))
fv,cfs=load(f'dump/{sys.argv[1]}.bin'); a=A(fv); e=a.c.e; ok=bad=0
for ref,t,v,fl,fid,b in cfs:
    if ref not in rows or t not in (0,1): continue
    for beg,end in rows[ref]:
        e.heap=a.heapmark; a.blen=len(b); a.overrun=False; a.reads=[]
        e.u.mem_write(BASE+0x2ed8000, a.snap_data); e.u.mem_write(TEB, a.snap_teb); e.u.mem_write(HEAP, a.snap_heap)
        buf=a.c.newbuf(b, fl, beg+2, t==1, False)
        # the pointer id tables Actor::LoadGame sets up (0xc8abd0): a fresh, empty one per actor
        tbl=e.alloc(0x60); obj2=e.alloc(0x40); e.u.mem_write(obj2+0x18, struct.pack('<Q', BASE+0xc8ac77+0x22a6e29))
        e.u.mem_write(TEB+0x20000+0x8d8, struct.pack('<Q', tbl)); e.u.mem_write(TEB+0x20000+0x8e8, struct.pack('<Q', obj2))
        typ=b[beg+1]
        try:
            obj=e.call(0xceaed0, typ)
            ctx=e.alloc(0x20); e.u.mem_write(ctx, struct.pack('<QQ', buf, 0))
            if obj:
                vt=struct.unpack('<Q',e.u.mem_read(obj,8))[0]; fn=struct.unpack('<Q',e.u.mem_read(vt+0x30,8))[0]
                e.call(fn-BASE, obj, ctx, limit=4000000)
            p=pos(e,buf)
        except Exception as ex: p=f'fault {ex} {e.r(UC_X86_REG_RIP)-BASE:x}'
        good = p==end; ok+=good; bad+=not good
        print(f'{ref:x} type {typ}: game end {p}, decoder end {end}', 'OK' if good else 'DIFF', flush=True)
print('same', ok, 'different', bad)
