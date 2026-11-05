import sys,re,struct;sys.path.insert(0,'.')
from pe import *
import numpy as np
def vtables(pattern):
    out={}
    q=np.frombuffer(data[:len(data)//8*8],dtype=np.uint64)
    d=np.frombuffer(data[:len(data)//4*4],dtype=np.uint32)
    for m in re.finditer(pattern, data):
        name=m.group(0).decode(); td=m.start()-0x10
        # COLs: signature 1, then offset, cdOffset, pTypeDescriptor(rva)
        for ci in np.nonzero(d==td)[0]:
            col=int(ci)*4-0x0c
            if col<0 or struct.unpack_from('<I',data,col)[0]!=1: continue
            off=struct.unpack_from('<I',data,col+4)[0]
            for vi in np.nonzero(q==base+col)[0]:
                vt=int(vi)*8+8
                out.setdefault(name,[]).append((vt,off))
    return out
if __name__=='__main__':
    v=vtables(sys.argv[1].encode())
    for k,l in sorted(v.items()):
        print(k, [(hex(a),o) for a,o in l])
