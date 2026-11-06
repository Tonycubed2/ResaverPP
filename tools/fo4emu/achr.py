# emulate Actor::LoadGame / PlayerCharacter::LoadGame on real ACHR bodies; records every read
import sys; sys.path.insert(0,'.')
from harness import *
from dec import refr as pyrefr, npc as pynpc, Unsupported
from cf import load
import collections, time
RDF={0xc0fd90:'R',0xc0fa10:'REF',0xc0fd30:'VS',0xc0fa90:'WS',0xc0fb90:'WS2',0xc0fdd0:'XF',0xc0ffb0:'R12'}
class A:
    def __init__(s, ver, fakeforms=True, inits=True, fakes=True):
        s.do_inits=inits; s.do_fakes=fakes
        s.c=Ctx(ver); s.ver=ver; e=s.c.e; s.reads=[]; s.limit_n=8000000
        def refr_stub(e2):
            buf=e2.r(UC_X86_REG_RDX)
            e2.u.mem_write(buf+0x24, struct.pack('<I', s.refr_end)); return 0
        e.stub(0x4f5c50, refr_stub)
        def setlevel(e2):
            actor=e2.r(UC_X86_REG_RCX); lvl=e2.r(UC_X86_REG_RDX)&0xffffffff
            if lvl==0xffffffff: return 0
            p=e2.alloc(0x100)
            e2.u.mem_write(p+0xdf, bytes([lvl&0xff]))
            e2.u.mem_write(p+0x48, struct.pack('<I',0xbf800000))
            if lvl<=2: e2.u.mem_write(p, struct.pack('<Q', e2.alloc(0x1000)))
            if lvl<=1: e2.u.mem_write(p+8, struct.pack('<Q', e2.alloc(0x1000)))
            if lvl==0: e2.u.mem_write(p+0x10, struct.pack('<Q', e2.alloc(0x2000)))
            e2.u.mem_write(actor+0x300, struct.pack('<Q', p)); return 0
        e.stub(0xc76700, setlevel)
        def nc(u,addr,size,ud):
            rsp=u.reg_read(UC_X86_REG_RSP); ra=struct.unpack('<Q',u.mem_read(rsp,8))[0]
            u.reg_write(UC_X86_REG_RSP, rsp+8); u.reg_write(UC_X86_REG_RIP, ra); u.reg_write(UC_X86_REG_RAX, 0)
        e.u.mem_protect(0, 0x10000, UC_PROT_READ|UC_PROT_EXEC)
        e.u.hook_add(UC_HOOK_CODE, nc, begin=0, end=0xffff)
        if fakeforms:
            s.forms={}
            def lookup(e2):
                fid=e2.r(UC_X86_REG_RCX)&0xffffffff
                if fid==0: return 0
                if fid not in s.forms: s.forms[fid]=e2.alloc(0x200)
                return s.forms[fid]
            e.stub(0x311b80, lookup)
        s.setup_factories()
        # record reads: at each reader's entry, the buffer position and the caller
        for a,n in RDF.items():
            def rec(e2,a=a,n=n):
                buf=e2.r(UC_X86_REG_RCX); rsp=e2.r(UC_X86_REG_RSP)
                ra=struct.unpack('<Q',e2.u.mem_read(rsp,8))[0]-BASE
                p=pos(e2,buf)
                s.reads.append((p, n, ra, e2.r(UC_X86_REG_R8)&0xffffffff))
                if p>s.blen: s.overrun=True; e2.emu_stop()
            e.u.hook_add(UC_HOOK_CODE, lambda u,addr,size,ud,rec=rec: rec(s.c.e), begin=BASE+a, end=BASE+a)
        def blob(u,addr,size,ud):
            e2=s.c.e; buf=e2.r(UC_X86_REG_RSI); p=pos(e2,buf)
            s.reads.append((p,'BLOB',0xc10b9f,e2.r(UC_X86_REG_R8)&0xffffffff))
        e.u.hook_add(UC_HOOK_CODE, blob, begin=BASE+0xc10b9f, end=BASE+0xc10b9f)
        def blob2(u,addr,size,ud):
            e2=s.c.e; buf=e2.r(UC_X86_REG_RSI); p=pos(e2,buf)
            s.reads.append((p,'BLOB',0xc10779,e2.r(UC_X86_REG_R8)&0xffffffff))
        e.u.hook_add(UC_HOOK_CODE, blob2, begin=BASE+0xc10779, end=BASE+0xc10779)
    def setup_factories(s):
        import pickle
        e=s.c.e; u=e.u
        s.fmap=pickle.load(open('factory.pkl','rb')); s.unknown=collections.Counter()
        s.strs={}; s.made=[]; s.initerr=[]
        def fixstr(e2):
            rcx=e2.r(UC_X86_REG_RCX); src=e2.r(UC_X86_REG_RDX)
            st=bytes(e2.u.mem_read(src, 512)).split(b'\0')[0] if src else b''
            if st not in s.strs:
                ent=e2.alloc(0x20+len(st)+1); e2.u.mem_write(ent+0x18, st+b'\0'); s.strs[st]=ent
            e2.u.mem_write(rcx, struct.pack('<Q', s.strs[st])); return rcx
        e.stub(0x167dd20, fixstr)
        # thread-safe statics: TLS epoch INT_MIN so guards run their initializers once
        u.mem_write(TEB+0x20000+0x1570, struct.pack('<I',0x80000000))
        def hdr(e2):
            g=e2.r(UC_X86_REG_RCX); v=struct.unpack('<i',e2.u.mem_read(g,4))[0]
            if v==0: e2.u.mem_write(g, struct.pack('<i',-1))
            return 0
        def ftr(e2):
            g=e2.r(UC_X86_REG_RCX); e2.u.mem_write(g, struct.pack('<i',1)); return 0
        e.stub(0x22befa8, hdr); e.stub(0x22bef48, ftr); e.stub(0x22bebcc, lambda e2: 0)  # atexit
        e.stub(0xc10e60, lambda e2: 0)   # stores a loaded blob in a map (no reads)
        e.stub(0x66c3e0, lambda e2: 0)   # quest stage lookup on a (fake) quest: no reads
        e.stub(0x66a290, lambda e2: 0)   # quest objective lookup: no reads
        e.stub(0x66e040, lambda e2: 0)   # a quest's entry by id (extra 0x88, 0x2ab92f) on a (fake) quest: no reads
        def newid(e2):  # 0xbedd20 (called by 0xcea430 only here): gives the created package its saved form id
            pkg=struct.unpack('<Q',e2.u.mem_read(e2.r(UC_X86_REG_RBX)+8,8))[0]
            e2.u.mem_write(pkg+0x14, struct.pack('<I', e2.r(UC_X86_REG_RDX)&0xffffffff)); return 0
        e.stub(0xbedd20, newid)
        # 0xe78b40: registers a loaded cover location (Havok class 0x4f117e2b, 0xe801a0, when its flag bit 12 is set) with the
        # cover manager singleton (0x330f910, not built here); called with the manager and the object, not the buffer
        e.stub(0xe78b40, lambda e2: 0)
        e.stub(0x2383e0, lambda e2: 0)  # a hash map insert reached only from created forms' constructors (no reads)
        if s.do_inits:
            import initall
            s.badinits=initall.run_inits(s)
        else:
            for f in (0x1c9ad0,0x1ca1d0,0x1ca6e0,0x1cc050,0x1cc1e0,0x7a7e0,0x7a910,0x7ab80,0x150480,0x1505d0,0x150720,0x1509c0,0x150ba0,0x150d20,0x151120,0x1ca390,0x1ca840,0x1caa10,0x1cb610,0x1cc490,0x1cc5c0,0x1cc9d0,0x1ccb00,0x1cd0b0,0x1cd290,0x1cd390,0x1cd490,0x1cd590,0x1cd690,0x1cd870,0x1cdb10,0x1cdc10,0x1cdd10,0x1cde70,0x1cdf70):
                try: e.call(f, limit=2000000)
                except UcError as ex: s.initerr.append((hex(f),hex(e.r(UC_X86_REG_RIP)-BASE)))
        # the player camera singleton with its camera states (PlayerCamera::LoadGame loads each state)
        g=BASE+0x30e6e58
        if not struct.unpack('<Q',u.mem_read(g,8))[0]:
            cam=e.alloc(0x400)
            try:
                e.call(0x1024f80, cam, limit=2000000); e.call(0x1025480, cam, 0, limit=2000000)
                u.mem_write(g, struct.pack('<Q',cam))
            except UcError: s.initerr.append('camera')
        # the Havok/pathing type registry (0x3271390: 0x40-byte entries {u32 type hash, ..., +0x30 LoadGame}; 0x32713a8 its
        # count, numbered by CRT initializers): filled by the game's own 0x89ce90, which the game runs from 0xed1b40 (a
        # pathing world constructor) and not from a CRT initializer; 0x89cb10 looks a saved type hash up in it
        if not struct.unpack('<Q',u.mem_read(BASE+0x3271390,8))[0]:
            try: e.call(0x89ce90, limit=50000000)
            except UcError: s.initerr.append('registry 0x89ce90')
        if s.do_fakes:
            sentinel=e.alloc(0x20)
            def build(names):
                ents=[e.alloc(0x20) for _ in names]
                for i,(nm,vt) in enumerate(names):
                    ent=s.intern(nm.encode())
                    creator=e.alloc(0x20); cvt=e.alloc(0x40); code=e.alloc(0x10)
                    u.mem_write(creator, struct.pack('<Q',cvt)); u.mem_write(cvt+8, struct.pack('<Q',code))
                    # package data classes with two bases: the game's own creators (they return the IPackageData
                    # base at +8, whose vtable +0x40 is the LoadGame)
                    REAL={'ObjectList':0x707fa0,'Ref':0x709a80,'RefOLD':0x702f80,'LocationWrapper':0x704eb0,'Location':0x702c20}
                    if nm in REAL:
                        u.mem_write(cvt+8, struct.pack('<Q',BASE+REAL[nm]))
                    else:
                        def make(e2, vt=vt, nm=nm):
                            o=e2.alloc(0x800); e2.u.mem_write(o, struct.pack('<Q',BASE+vt)); s.made.append(nm); return o
                        e.stub(code-BASE, make)
                    nxt=ents[i+1] if i+1<len(ents) else sentinel
                    u.mem_write(ents[i], struct.pack('<QQQ', ent, creator, nxt))
                tbl=e.alloc(0x20); u.mem_write(tbl, bytes(u.mem_read(ents[0],0x18)))
                return tbl
            tbl=build(list(s.fmap.items()))
            g1=BASE+0x32174c0
            u.mem_write(g1+0xc, struct.pack('<I',1)); u.mem_write(g1+0x18, struct.pack('<Q',sentinel)); u.mem_write(g1+0x1820, struct.pack('<Q',tbl))
            e.stub(0x7106f0, lambda e2: g1)
            g2=e.alloc(0x400)
            u.mem_write(g2+0xc, struct.pack('<I',1)); u.mem_write(g2+0x18, struct.pack('<Q',sentinel)); u.mem_write(g2+0x1a0, struct.pack('<Q',tbl))
            e.stub(0x21c2b0, lambda e2: g2)
        # class names the procedure states look up as fixed strings, set by functions run on demand in the game
        # (0x70a700 and others), not by the CRT initializers: Activate, Dialogue, Travel, UseIdleMarker, SitSleep
        for g,nm in ((0x3218d98,b'Activate'),(0x3218da8,b'Dialogue'),(0x30e6e90,b'Travel'),(0x3218ee0,b'UseIdleMarker'),(0x3218ed8,b'SitSleep')):
            u.mem_write(BASE+g, struct.pack('<Q', s.intern(nm)))
        s.heapmark=e.heap
        s.snap_data=bytes(u.mem_read(BASE+0x2ed8000, 0xf9e000))
        s.snap_teb=bytes(u.mem_read(TEB, 0x100000))
        s.snap_heap=bytes(u.mem_read(HEAP, s.heapmark-HEAP))
        s.strs0=dict(s.strs)
    def intern(s, st):
        e=s.c.e
        if st not in s.strs:
            ent=e.alloc(0x20+len(st)+1); e.u.mem_write(ent+0x18, st+b'\0'); s.strs[st]=ent
        return s.strs[st]
    def parse(s, ref, fl, b):
        s.reads=[]; s.blen=len(b); s.overrun=False
        s.refr_end=pyrefr(b, ref, fl, s.ver, achr=True, npc_fn=lambda r,f: pynpc(r,f,s.ver))
        c=s.c; e=c.e; e.heap=s.heapmark; s.forms={}; s.made=[]
        e.u.mem_write(BASE+0x2ed8000, s.snap_data); e.u.mem_write(TEB, s.snap_teb); e.u.mem_write(HEAP, s.snap_heap); s.strs=dict(s.strs0)
        r=R(b); initial(r, initial_type(ref,fl)); havok(r,fl)
        player = ref in (0x14,0x400014)
        buf=c.newbuf(b, fl, r.p, True, player); obj=c.obj
        lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8)); e.u.mem_write(obj+0x100, struct.pack('<Q', lst))
        npc=e.alloc(0x400); e.u.mem_write(npc, struct.pack('<Q', BASE+0x24d6400)); e.u.mem_write(npc+0x1a, bytes([0x2d]))
        e.u.mem_write(obj+0xe0, struct.pack('<Q', npc))
        s.buf=buf
        e.call(0xd65bd0 if player else 0xc8abd0, obj, buf, limit=s.limit_n)
        if e.r(UC_X86_REG_RIP)!=RET: s.limit=True; raise UcError(UC_ERR_OK)
        return pos(e,buf)
def run(fn, lim=10**9, only=None):
    fv,cfs=load(f'dump/{fn}.bin'); a=A(fv); st=collections.Counter(); why=collections.Counter(); n=0; t0=time.time()
    for ref,t,v,fl,fid,b in cfs:
        if t!=1: continue
        if only is not None and fid!=only: continue
        n+=1
        if n>lim: break
        try:
            p=a.parse(ref,fl,b)
            st['exact' if p==len(b) else 'short' if p<len(b) else 'over']+=1
            if p!=len(b): why[('short' if p<len(b) else 'over', a.reads[-1][2] if a.reads else None)]+=1
        except (Unsupported,EOFError) as ex: st['pyrefr']+=1
        except UcError as ex:
            st['fault']+=1; why[(hex(a.c.e.r(UC_X86_REG_RIP)-BASE), tuple(backtrace(a.c.e,4)))]+=1
    print(fn, dict(st), f'{time.time()-t0:.0f}s'); [print('  ',v,k) for k,v in why.most_common(12)]
    return a
if __name__=='__main__':
    run(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else 10**9)
