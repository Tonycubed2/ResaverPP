import struct,collections
def load(p):
    b=open(p,'rb').read(); fv=struct.unpack_from('<I',b,0)[0]; o=4; out=[]
    while o<len(b):
        ref,t,v,fl,fid,n=struct.unpack_from('<IBBIII',b,o); o+=18
        out.append((ref,t,v,fl,fid,b[o:o+n])); o+=n
    return fv,out
