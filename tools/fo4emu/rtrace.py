import struct
def rtrace(fn):
    d=open(f'trace_{fn}.bin','rb').read(); n=struct.unpack_from('<I',d,0)[0]; p=4; out=[]
    for _ in range(n):
        ref,ex,st,end,k=struct.unpack_from('<IBIII',d,p); p+=17
        rs=[struct.unpack_from('<IBI',d,p+9*i) for i in range(k)]; p+=9*k
        out.append((ref,ex,st,end,rs))
    return out
