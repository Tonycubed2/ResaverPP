import struct, pickle, sys
from unicorn import *
from unicorn.x86_const import *
import pefile
BASE=0x140000000
_pe=pefile.PE('Fallout4.exe',fast_load=True)
IMG=_pe.get_memory_mapped_image()
SIZE=(len(IMG)+0xfff)&~0xfff
IMPORTS=pickle.load(open('imports.pkl','rb'))
STACK=0x7f000000; STACKSZ=0x400000
HEAP=0x10000000; HEAPSZ=0x10000000
STUB=0x20000000  # import stubs
TEB=0x30000000
RET=0x31000000
class Emu:
    def __init__(s, version=68):
        u=Uc(UC_ARCH_X86, UC_MODE_64); s.u=u; s.version=version
        u.mem_map(BASE, SIZE); u.mem_write(BASE, bytes(IMG))
        u.mem_map(STACK, STACKSZ); u.mem_map(HEAP, HEAPSZ); u.mem_map(STUB, 0x100000); u.mem_map(TEB,0x100000); u.mem_map(RET,0x1000)
        u.mem_write(RET, b'\xc3')
        # null objects: a readable, writable, not executable zero page at 0; calling through a null vtable returns 0
        u.mem_map(0, 0x10000, UC_PROT_READ)
        import capstone as _cs
        _md=_cs.Cs(_cs.CS_ARCH_X86, _cs.CS_MODE_64)
        def nullwrite(uc, access, address, size, value, ud):
            if address < 0x10000:
                rip=uc.reg_read(UC_X86_REG_RIP)
                ins=next(_md.disasm(bytes(uc.mem_read(rip,16)), rip))
                uc.reg_write(UC_X86_REG_RIP, rip+ins.size)   # a write to a null object: skipped
                s.trace.append(('nullwrite', rip)); return True
            return False
        u.hook_add(UC_HOOK_MEM_WRITE_PROT, nullwrite)
        def nullcall(uc, access, address, size, value, ud):
            if address < 0x10000:
                rsp=uc.reg_read(UC_X86_REG_RSP); ra=struct.unpack('<Q',uc.mem_read(rsp,8))[0]
                uc.reg_write(UC_X86_REG_RSP, rsp+8); uc.reg_write(UC_X86_REG_RIP, ra); uc.reg_write(UC_X86_REG_RAX, 0)
                s.trace.append(('nullcall', address)); return True
            return False
        u.hook_add(UC_HOOK_MEM_FETCH_PROT, nullcall)
        s.heap=HEAP+0x1000
        # imports -> stubs (each stub is 'ret'); hooked by address
        s.impstub={}
        k=0
        for rva,(dll,name) in IMPORTS.items():
            a=STUB+k*16; k+=1
            u.mem_write(a, b'\xc3'); u.mem_write(BASE+rva, struct.pack('<Q',a)); s.impstub[a]=name
        # TLS: gs:[0x58] -> array of 64 ptrs to a zeroed block
        tls=TEB+0x10000
        u.mem_write(TEB+0x58, struct.pack('<Q', tls))
        for i in range(64): u.mem_write(tls+8*i, struct.pack('<Q', TEB+0x20000))
        u.reg_write(UC_X86_REG_GS_BASE, TEB)
        s.stubs={}   # rva -> python fn(emu) returning rax
        s.trace=[]
        u.hook_add(UC_HOOK_CODE, s._hook_stub, begin=STUB, end=STUB+0x100000)
        s.hooked=set()
    def alloc(s, n, zero=True):
        if n>0x1000000 or n<0: s.trace.append(('bigalloc',n)); n=0x1000
        n=(n+15)&~15
        if s.heap+n+16>HEAP+HEAPSZ-0x100000: s.heap=HEAP+0x1000
        a=s.heap; s.heap+=n+16
        s.u.mem_write(a, b'\0'*(n+16))
        return a
    def r(s,reg): return s.u.reg_read(reg)
    def ret(s, v):
        u=s.u; rsp=u.reg_read(UC_X86_REG_RSP); ra=struct.unpack('<Q',u.mem_read(rsp,8))[0]
        u.reg_write(UC_X86_REG_RSP, rsp+8); u.reg_write(UC_X86_REG_RIP, ra); u.reg_write(UC_X86_REG_RAX, v)
    def _hook_stub(s,u,addr,size,ud):
        name=s.impstub.get(addr)
        if name is None: return
        rcx,rdx,r8=s.r(UC_X86_REG_RCX),s.r(UC_X86_REG_RDX),s.r(UC_X86_REG_R8)
        if name in ('memcpy','memmove'):
            if r8: u.mem_write(rcx, bytes(u.mem_read(rdx, r8)))
            v=rcx
        elif name=='memset':
            if r8: u.mem_write(rcx, bytes([rdx&0xff])*r8)
            v=rcx
        elif name=='__RTDynamicCast': v=rcx
        else:
            s.trace.append(('import',name)); v=0
        s.ret(v)
    def stub(s, rva, fn):
        a=BASE+rva
        if a not in s.hooked:
            s.hooked.add(a)
            s.u.hook_add(UC_HOOK_CODE, lambda u,addr,size,ud: s._fire(addr), begin=a, end=a)
        s.stubs[a]=fn
    def emu_stop(s): s.u.emu_stop()
    def _fire(s,addr):
        fn=s.stubs.get(addr)
        if fn: s.ret(fn(s) or 0)
    def call(s, rva, *args, limit=2000000):
        u=s.u
        rsp=STACK+STACKSZ-0x1000
        rsp-=8; u.mem_write(rsp, struct.pack('<Q', RET))
        for reg,a in zip((UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_R8,UC_X86_REG_R9),args): u.reg_write(reg,a)
        u.reg_write(UC_X86_REG_RSP, rsp)
        u.emu_start(BASE+rva, RET, count=limit)
        return u.reg_read(UC_X86_REG_RAX)

def backtrace(e, n=12):
    rsp=e.r(UC_X86_REG_RSP); out=[]
    for k in range(0, 0x800, 8):
        try: v=struct.unpack('<Q', e.u.mem_read(rsp+k,8))[0]
        except: break
        if BASE+0x1000<=v<BASE+0x2435000:
            # is it right after a call?
            for back in (5,6,2,3,7):
                try:
                    b=bytes(e.u.mem_read(v-back,back))
                except: continue
                if (back==5 and b[0]==0xe8) or (back==6 and b[:2]==b'\xff\x15') or (back in(2,3,7) and b[0]==0xff):
                    out.append(hex(v-BASE)); break
        if len(out)>=n: break
    return out
