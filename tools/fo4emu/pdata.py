import struct, bisect
from pe import data
PD=0x3e76000; PDN=0x27ef88//12
_s=[]; _e=[]
for i in range(PDN):
    b,e,u=struct.unpack_from('<III',data,PD+12*i)
    if b: _s.append(b); _e.append(e)
def func_of(rva):
    i=bisect.bisect_right(_s,rva)-1
    return (_s[i],_e[i]) if i>=0 and _s[i]<=rva<_e[i] else None
