# the type map extra data 0xa9 (0xdbf560) looks its u32 hash up in: entries at [0x32737a0], capacity [0x32736cc]
import sys,struct;sys.path.insert(0,'.')
from achr import *
a=A(68); u=a.c.e.u
def q(x): return struct.unpack('<Q',u.mem_read(x,8))[0]
ent=q(BASE+0x32737a0); cap=struct.unpack('<I',u.mem_read(BASE+0x32736cc,4))[0]; sent=q(BASE+0x32736d8)
print('entries',hex(ent),'cap',cap,'sentinel',hex(sent))
if ent:
    for i in range(cap):
        k,c,n=struct.unpack('<QQQ',u.mem_read(ent+24*i,24))
        if not n: continue
        create=q(q(c)+8)-BASE
        print(hex(k&0xffffffff), 'creator', hex(c), 'create', hex(create))
