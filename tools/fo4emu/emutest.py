import sys; sys.path.insert(0,'.')
from emu import *
def make(e, body, flags, version):
    buf=e.alloc(0x200)
    data=e.alloc(len(body)+64); e.u.mem_write(data, body)
    vt=e.alloc(0x200)
    # vtable: all slots -> ret 0 stub; slot 1 -> version
    retz=e.alloc(16); e.u.mem_write(retz, b'\x31\xc0\xc3')
    retv=e.alloc(16); e.u.mem_write(retv, b'\xb8'+struct.pack('<I',version)+b'\xc3')
    for i in range(64): e.u.mem_write(vt+8*i, struct.pack('<Q', retz))
    e.u.mem_write(vt+8, struct.pack('<Q', retv))
    e.u.mem_write(buf, struct.pack('<Q', vt))
    e.u.mem_write(buf+8, struct.pack('<Q', data))
    e.u.mem_write(buf+0x24, struct.pack('<I', 0))
    e.u.mem_write(buf+0x40, struct.pack('<I', flags))
    return buf
def pos(e,buf): return struct.unpack('<I', e.u.mem_read(buf+0x24,4))[0]
def common_stubs(e):
    e.stub(0x1e0320, lambda e: e.alloc(e.r(UC_X86_REG_RCX)))     # alloc
    e.stub(0x311b80, lambda e: 0)                                 # lookup form by id
    def refid(e):
        p=e.r(UC_X86_REG_RCX); b=bytes(e.u.mem_read(p,3)); v=b[0]<<16|b[1]<<8|b[2]
        if (v & 0xc00000) == 0x800000: return 0xff000000 | (v & 0x3fffff)  # a created form (0xc0af70)
        return (v & 0x3fffff) | 0x01000000 if v else 0
    e.stub(0xc0af70, refid)
    for a in (0x1659480,0x16596e0,0x1659400,0x16596d0,0x16583b0):
        e.stub(a, lambda e: 0)
    e.stub(0x1657de0, lambda e: e.alloc(e.r(UC_X86_REG_RDX) or 64))
    e.stub(0x1658240, lambda e: 0)          # free
    e.stub(0x2a0a70, lambda e: 0)           # handle -> object lookup (no objects here)
    e.stub(0x312680, lambda e: 0)           # remove a form from the global form map
    e.stub(0x1657250, lambda e: HEAP+0x100)  # a heap
    e.stub(0x165c240, lambda e: e.alloc(e.r(UC_X86_REG_RDX) or 16))  # heap alloc
    def fixstr(e):
        rcx=e.r(UC_X86_REG_RCX); src=e.r(UC_X86_REG_RDX)
        st=b''
        if src:
            st=bytes(e.u.mem_read(src, 512)).split(b'\0')[0]
        ent=e.alloc(0x20+len(st)+1); e.u.mem_write(ent+0x18, st+b'\0')
        e.u.mem_write(rcx, struct.pack('<Q', ent)); return rcx
    e.stub(0x167dd20, fixstr)               # BSFixedString pool insert
    e.stub(0x167d4a0, lambda e: 0)          # BSFixedString release
if __name__=='__main__':
    e=Emu(); common_stubs(e)
    body=bytes.fromhex(sys.argv[1]); 
    lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8))
    buf=make(e, body, int(sys.argv[2],0), 68)
    try:
        e.call(0x294760, lst, buf)
        print('consumed', pos(e,buf), 'of', len(body))
    except UcError as ex:
        print('fault', ex, hex(e.r(UC_X86_REG_RIP)-BASE))
    print(e.trace[:10])
