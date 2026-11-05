import sys; sys.path.insert(0,'.')
from emu import *
from emutest import make, pos, common_stubs
from proto import R, bit, initial_type, initial, havok
import struct
EXTRA_MASK_REF=0xa6021f40; EXTRA_MASK_ACT=0xa6061b40
class Ctx:
    def __init__(s, version):
        s.e=Emu(version); common_stubs(s.e); s.version=version
        s.e.heap0=s.e.heap
    def reset(s):
        s.e.heap=s.e.heap0
    def newbuf(s, body, flags, at, achr=False, player=False):
        e=s.e
        buf=make(e, body, flags, s.version)
        # the form being loaded (some extra data is only read when the buffer has it)
        obj=e.alloc(0x400)
        vt=0x256c938 if player else 0x25683b0 if achr else 0x24a4c28
        e.u.mem_write(obj, struct.pack('<Q', BASE+vt)); e.u.mem_write(obj+0x1a, bytes([0x41 if achr else 0x40]))
        e.u.mem_write(obj+0x14, struct.pack('<I', 0x14 if player else 0xff000800))
        e.u.mem_write(buf+0x38, struct.pack('<Q', obj))
        s.obj=obj
        e.u.mem_write(buf+0x24, struct.pack('<I', at))
        return buf
    def extra(s, buf):
        e=s.e; lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8))
        e.call(0x294760, lst, buf)
    def inventory(s, buf):
        e=s.e; inv=e.alloc(0x200)
        e.call(0x34ee00, inv, buf)
def refr_part(c, ref, flags, body, achr=False):
    """the reference part; returns buffer object positioned after it"""
    r=R(body)
    initial(r, initial_type(ref,flags)); havok(r,flags)
    buf=c.newbuf(body, flags, r.p, achr, ref==0x14 or ref==0x400014)
    e=c.e
    p=pos(e,buf)
    if bit(flags,0): p+=6
    if bit(flags,7): p+=3
    if bit(flags,4): p+=4
    e.u.mem_write(buf+0x24, struct.pack('<I',p))
    mask=EXTRA_MASK_ACT if achr else EXTRA_MASK_REF
    if flags & mask: c.extra(buf)
    if flags & 0x8000020: c.inventory(buf)
    p=pos(e,buf)
    if bit(flags,28):
        rr=R(body); rr.p=p; rr.u8(); n=rr.vs(); rr.raw(n); p=rr.p
        e.u.mem_write(buf+0x24, struct.pack('<I',p))
    return buf

def full_native(c, ref, flags, body, achr=False):
    """initial + havok in python, then the game's own LoadGame on a fake object"""
    r=R(body)
    initial(r, initial_type(ref,flags)); havok(r,flags)
    player = ref in (0x14, 0x400014)
    buf=c.newbuf(body, flags, r.p, achr, player)
    e=c.e; obj=c.obj
    # the object's extra data list and inventory list
    lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8))
    e.u.mem_write(obj+0x100, struct.pack('<Q', lst))
    inv=e.alloc(0x200); e.u.mem_write(obj+0xf8, struct.pack('<Q', inv))
    fn = 0xd65bd0 if player else 0xc8abd0 if achr else 0x4f5c50
    e.call(fn, obj, buf)
    return buf

def refr_body(c, body, flags, at, achr, player):
    """the REFR LoadGame part from position 'at' (form flags, base, scale, extra, inventory, animation); returns new position"""
    e=c.e
    buf=c.newbuf(body, flags, at, achr, player)
    p=at
    if bit(flags,0): p+=6
    if bit(flags,7): p+=3
    if bit(flags,4): p+=4
    e.u.mem_write(buf+0x24, struct.pack('<I',p))
    mask=EXTRA_MASK_ACT if achr else EXTRA_MASK_REF
    if flags & mask: c.extra(buf)
    if flags & 0x8000020: c.inventory(buf)
    p=pos(e,buf)
    if bit(flags,28):
        rr=R(body); rr.p=p; rr.u8(); n=rr.vs(); rr.raw(n); p=rr.p
    return p

class ActorCtx:
    def __init__(s, version):
        s.main=Ctx(version); s.aux=Ctx(version); s.version=version
        e=s.main.e
        def refr_stub(e2):
            buf=e2.r(UC_X86_REG_RDX)
            p=pos(e2,buf)
            s.aux.reset()
            try:
                np=refr_body(s.aux, s.body, s.flags, p, True, s.player)
            except Exception as ex:
                s.auxerr=(repr(ex)[:40], hex(s.aux.e.r(UC_X86_REG_RIP)-BASE), tuple(backtrace(s.aux.e,4)))
                e2.emu_stop(); return 0
            e2.u.mem_write(buf+0x24, struct.pack('<I', np))
            s.refr_end=np
            return 0
        e.stub(0x4f5c50, refr_stub)
    def reset(s): s.main.reset()
    def parse(s, ref, flags, body):
        s.body=body; s.flags=flags; s.player = ref in (0x14,0x400014)
        r=R(body); initial(r, initial_type(ref,flags)); havok(r,flags)
        c=s.main; e=c.e
        buf=c.newbuf(body, flags, r.p, True, s.player)
        obj=c.obj
        lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8)); e.u.mem_write(obj+0x100, struct.pack('<Q', lst))
        npc=e.alloc(0x400); e.u.mem_write(npc, struct.pack('<Q', BASE+0x24d6400)); e.u.mem_write(npc+0x1a, bytes([0x2d]))
        e.u.mem_write(obj+0xe0, struct.pack('<Q', npc))
        s.refr_end=None; s.auxerr=None
        e.stub(0xc76700, lambda e: 0)
        e.call(0xd65bd0 if s.player else 0xc8abd0, obj, buf)
        if s.auxerr: raise RuntimeError('aux '+str(s.auxerr))
        return buf
