# reference decoder for Fallout 4 REFR / ACHR change form bodies (python; ported to C++ afterwards)
import struct
from proto import R, bit, initial_type, initial, havok
class Unsupported(Exception): pass
def ws(r): n=r.u16(); return r.raw(n)
FIXED={0x06:0,0x08:0,0x17:2,0x18:19,0x19:13,0x1c:6,0x1d:0,0x1e:4,0x1f:1,0x21:7,0x24:2,0x25:4,0x2c:1,0x2e:4,0x31:28,
       0x32:4,0x34:1,0x38:3,0x44:1,0x48:3,0x49:1,0x54:1,0x55:4,0x58:7,0x5d:4,0x65:3,0x70:3,0x85:3,0x87:20,0x89:3,0x8b:3,
       0x92:3,0x9f:5,0xa0:4,0xa1:88,0xb0:8,0xbb:4,0xc0:12,0xc3:3,0xc4:3,0xc9:4,0xcd:0,0x95:7,0x8e:3,0x68:3,0x2f:4,0x23:4,0x67:32,0x82:12,0x22:3,0x26:8,0x27:4,0x28:4,0x53:4,0x59:4,0x6a:4,0x4d:1,0x4f:2,0x9c:1,0x9b:5,0x3e:7}
DEFAULT={0xd,0xe,0xf,0x10,0x11,0x12,0x13,0x14,0x15,0x20,0x29,0x30,0x33,0x36,0x37,0x39,0x3a,0x3b,0x3c,0x3f,0x40,0x41,0x42,0x43,0x46,0x47,0x4a,0x4b,0x4c,0x4e,0x51,0x52,0x56,0x57,0x5a,0x5e,0x5f,0x60,0x61,0x62,0x63,0x64,0x66,0x69,0x6b,0x6d,0x6e,0x72,0x73,0x74,0x75,0x76,0x77,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80,0x81,0x83,0x84,0x86,0x8a,0x8d,0x8f,0x90,0x91,0x93,0x94,0x96,0x97,0x9a,0x9e,0xa2,0xa3,0xa5,0xa6,0xa7,0xa8,0xaa,0xab,0xad,0xb1,0xb2,0xb3,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbc,0xbe,0xc5,0xc7,0xca,0xcb,0xcc,0xcd,0xce,0xd1,0xd2,0xd4}
def extra_entry(r, ty, ver, npc_fn=None, top=True):
    if ty in FIXED: r.raw(FIXED[ty]); return
    if ty==0xac:
        if top: r.ref()   # (only read when the buffer has the reference being loaded: not for inventory items)
        return
    if ty==0x2a:  # lock
        r.raw(1); r.raw(1); r.ref(); r.raw(4)
        if ver<0x36: r.raw(4)
    elif ty==0x0c:
        n=r.vs(); r.raw(3*n); m=r.vs(); r.raw(m)
    elif ty==0xbf:   # workshop (0x388bc0)
        n=r.u32()
        for i in range(n):
            m=r.u32(); r.raw(9*m); r.raw(8)
            if ver>=0x37: k=r.vs(); r.raw(3*k)
        p=r.u32()
        for i in range(p):
            if ver>=0x42: r.ref()
            else: r.raw(4)
            r.raw(4)
    elif ty==0xb4:
        if ver>=0x19: r.raw(24)
        else: raise Unsupported('extra 0xb4 old')
    elif ty==0xd0:
        if ver>=0x3f: n=r.vs(); r.raw(7*n)
        else: raise Unsupported('extra 0xd0 old')
    elif ty==0x8c: n=r.vs(); r.raw(3*n)
    elif ty==0x88: n=r.vs(); r.raw(7*n)
    elif ty==0x50: n=r.vs(); r.raw(6*n)
    elif ty==0x5b: n=r.vs(); r.raw(4*n); r.raw(4)
    elif ty==0xc6: n=r.vs(); r.raw(4*n)
    elif ty==0xc1:
        n=r.vs()
        for i in range(n): ws(r)
    elif ty==0xcf:
        n=r.vs()
        for i in range(n): ws(r); ws(r); r.raw(4)
    elif ty==0xc2:
        n=r.vs(); r.raw(7*n); m=r.vs(); r.raw(15*m)
    elif ty==0x78: n=r.vs(); r.raw(12*n)
    elif ty==0x35:
        r.raw(2); n=r.u32()
        r.raw(n*(6 if ver>=0x15 else 5))
    elif ty==0x99:
        msg=r.ref(); quest=r.ref(); val=struct.unpack('<i',r.raw(4))[0]; r.raw(2)
        if msg==0 and quest==0 and val==-2: ws(r)
        n=r.vs()
        for i in range(n): ws(r); r.ref()
    elif ty==0x5c:
        r.raw(2); r.raw(4); r.raw(4); r.raw(1); r.ref()
        n=r.vs()
        for i in range(n):
            if ver>=0x17: r.raw(4)
            r.raw(4)
            if ver>=0x1e: r.raw(1)
            ws(r)
            for j in range(r.vs()): r.ref(); r.raw(4)   # 0x437100 -> 0x436f00: form id, u32 (nothing after the entries)
    elif ty==0x98:   # 0x2ada30/0x2ae0b0; 0xc0fdd0 packs a 48-byte transform into 16 bytes
        n=r.vs()
        for i in range(n):
            if r.ref()==0: continue    # the game reads the rest only when the form resolves
            k=r.u16()
            if k<0xffff: r.raw(8); r.raw(12); r.raw(16); r.raw(4)
        r.raw(4)
    elif ty==0x6c:
        b=r.u8()
        if b!=0xff: r.raw(6); ws(r)
    elif ty==0x2d:
        r.ref(); r.raw(13*3); r.ref(); fl=r.u32()
        if fl:
            if npc_fn is None: raise Unsupported('npc in 0x2d')
            npc_fn(r, fl)
    elif ty<0x0c or ty>0xd5 or ty in DEFAULT:
        return   # the game's default case: logged, nothing read
    else:
        raise Unsupported(f'extra {ty:#x}')
def extra_list(r, ver, npc_fn=None, out=None, top=True):
    n=r.vs()
    for i in range(n):
        st=r.p; ty=r.u8()
        extra_entry(r, ty, ver, npc_fn, top)
        if out is not None: out.append((ty, st, r.p))
def inventory(r, ver, npc_fn=None, items=None):
    n=r.u32()
    for i in range(n):
        item=r.ref(); stacks=r.u32()
        for k in range(stacks):
            ex=[]
            extra_list(r, ver, npc_fn, ex, top=False)
            cnt=struct.unpack('<i', r.raw(4))[0]; fl=r.u16()
            if items is not None: items.append((item, cnt, fl, ex))
EXTRA_MASK_REF=0xa6021f40; EXTRA_MASK_ACT=0xa6061b40
def refr(body, ref, flags, ver, achr=False, npc_fn=None, items=None):
    r=R(body)
    initial(r, initial_type(ref,flags)); havok(r,flags)
    if achr: r.raw(8)
    if bit(flags,0): r.raw(6)
    if bit(flags,7): r.ref()
    if bit(flags,4): r.raw(4)
    mask=EXTRA_MASK_ACT if achr else EXTRA_MASK_REF
    if flags & mask: extra_list(r, ver, npc_fn)
    if flags & 0x8000020: inventory(r, ver, npc_fn, items)
    if bit(flags,28): r.u8(); n=r.vs(); r.raw(n)
    return r.p

def npc(r, flags, ver):
    """TESNPC::LoadGame (0x65bae0) after TESActorBase::LoadGame (0x644dd0)"""
    if bit(flags,0): r.raw(6)
    if bit(flags,1): r.raw(20)                      # ACBS
    if bit(flags,6): n=r.vs(); r.raw(4*n)           # factions: ref + rank
    if bit(flags,4):                                # spells, leveled spells, shouts
        for k in range(3): n=r.vs(); r.raw(3*n)
    if bit(flags,3): r.raw(24)                      # AI data
    if bit(flags,5): ws(r)                          # full name
    if ver>=0x20 and bit(flags,24): r.raw(1)        # gender
    if bit(flags,9): r.raw(8)                       # skills
    if bit(flags,10): r.ref()                       # class
    if bit(flags,25): r.ref(); r.ref()              # race, original race
    if bit(flags,11):                               # face
        if r.u8():
            r.ref()
            if ver>=0x40: r.ref()
            r.raw(4); r.ref()
        n=r.vs(); r.raw(3*n)                        # head parts
        if r.u8():
            c=r.u32(); r.raw(8*c)
        m=r.u32(); r.raw(40*m)                      # morphs
        k=r.u32(); r.raw(10*k)
    if bit(flags,14):                               # body morph region values
        n=r.u32(); r.raw(4*n); r.raw(12)
    if ver<0x20 and bit(flags,24): r.raw(1)
    if bit(flags,12): r.ref()                       # default outfit
    if bit(flags,13): r.ref()                       # sleep outfit
