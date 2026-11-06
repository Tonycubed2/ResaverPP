# the game's own TESObjectREFR::LoadGame (0x4f5c50, Fallout4.exe 1.11.240) on edited workshop REFR bodies (wsdump.cpp):
# reads to the exact end of the body, ActorValueStorage::LoadGame (0xcedc80) is entered once, and the (actor value
# ref, f32 base) pairs it reads are the ones expected. usage (from the fo4emu folder): wsemu.py <dump.bin> <expected.txt>
import sys,struct; sys.path.insert(0,'.')
from harness import *
from cf import load
from proto import R, bit, initial_type, initial, havok
fv,cfs=load(sys.argv[1])
exp={}
for line in open(sys.argv[2]):
    p=line.split(); exp[int(p[0],16)]=[(int(a,16),int(b,16)) for a,b in (x.split(':') for x in p[2:])]
c=Ctx(fv); e=c.e
got=[]; cur=[None]; entered=[0]
def at_lookup(u,addr,size,ud): cur[0]=e.r(UC_X86_REG_RCX)   # 0xcedd63 (after mov ecx,[rbp+0x67]): the resolved form id of the actor value ref
def at_base(u,addr,size,ud):                               # 0xcedd9d: after reading the f32 base into [rbp+0x7f]
    rbp=e.r(UC_X86_REG_RBP); bits=struct.unpack('<I',bytes(u.mem_read(rbp+0x7f,4)))[0]; got.append((cur[0],bits))
def at_enter(u,addr,size,ud): entered[0]+=1
e.u.hook_add(UC_HOOK_CODE, at_lookup, begin=BASE+0xcedd63, end=BASE+0xcedd63)
e.u.hook_add(UC_HOOK_CODE, at_base, begin=BASE+0xcedd9d, end=BASE+0xcedd9d)
e.u.hook_add(UC_HOOK_CODE, at_enter, begin=BASE+0xcedc80, end=BASE+0xcedc80)
ok=bad=0
for ref,t,v,fl,fid,b in cfs:
    if t!=0: continue
    c.reset(); got.clear(); entered[0]=0
    r=R(b); initial(r, initial_type(ref,fl)); havok(r,fl)
    try:
        buf=c.newbuf(b, fl, r.p, False, False); obj=c.obj
        lst=e.alloc(0x100); e.u.mem_write(lst+0x10, struct.pack('<Q', lst+8)); e.u.mem_write(obj+0x100, struct.pack('<Q', lst))
        inv=e.alloc(0x200); e.u.mem_write(obj+0xf8, struct.pack('<Q', inv))
        e.call(0x4f5c50, obj, buf)
        end=pos(e,buf)
    except UcError as ex:
        print(f'{fid:08X} fault {e.r(UC_X86_REG_RIP)-BASE:x}'); bad+=1; continue
    # the emulator's REFID stub gives (v & 0x3fffff) | 0x01000000 for a raw ref v
    want=[((a & 0x3fffff) | 0x01000000, bits) for a,bits in exp[ref]]
    good = end==len(b) and entered[0]==1 and got==want
    if good: ok+=1
    else:
        bad+=1; print(f"{fid:08X} end {end}/{len(b)} entered {entered[0]} values", "ok" if got==want else "DIFFER", [(hex(a),hex(x)) for a,x in got[:3]], [(hex(a),hex(x)) for a,x in want[:3]])
print(sys.argv[1], f'{ok} exact with the expected values, {bad} not')
