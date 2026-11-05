import struct
class R:
    def __init__(s,b): s.b=b; s.p=0
    def rem(s): return len(s.b)-s.p
    def u8(s): v=s.b[s.p]; s.p+=1; return v
    def u16(s): v=struct.unpack_from('<H',s.b,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from('<I',s.b,s.p)[0]; s.p+=4; return v
    def f32(s): v=struct.unpack_from('<f',s.b,s.p)[0]; s.p+=4; return v
    def ref(s): v=s.b[s.p]<<16|s.b[s.p+1]<<8|s.b[s.p+2]; s.p+=3; return v
    def raw(s,n):
        if n<0 or s.p+n>len(s.b): raise EOFError
        v=s.b[s.p:s.p+n]; s.p+=n; return v
    def vs(s):
        a=s.b[s.p]
        k=a&3
        if k==0: s.p+=1; return a>>2
        if k==1: v=struct.unpack_from('<H',s.b,s.p)[0]; s.p+=2; return v>>2
        v=struct.unpack_from('<I',s.b,s.p)[0]&0xffffff; s.p+=3; return v>>2
def bit(f,n): return (f>>n)&1
def initial_type(ref,fl):
    if (ref>>22)==2: return 5
    if bit(fl,25) or bit(fl,3): return 6
    if bit(fl,2) or bit(fl,1): return 4
    return 0
def initial(r,t):
    if t==4: r.raw(27)
    elif t==5: r.raw(27); r.u8(); r.ref()
    elif t==6: r.raw(27); r.ref(); r.raw(4)
def havok(r,fl):
    if bit(fl,2): r.u8(); r.raw(r.vs())
