import sys;sys.path.insert(0,'.')
from fsum import walk
from pe import *
RD={0xc0fd90:'READ',0xc0fa10:'REFID',0xc0fd30:'VSVAL',0xc0fa90:'WSTR',0xc0fb90:'WSTR2',0xc0fdd0:'XFORM'}
SKIP={0x1657de0,0x16583b0,0x1658240,0x311b80,0x22c46ae,0x1e0320,0x167dd20,0x167c110,0x7106f0,0x167d4a0}
memo={}
def psum(fn, depth=0, maxd=4):
    key=(fn,depth)
    out=[]
    last={}
    for i in walk(fn,fn,fn+0x3000,set()):
        if i.mnemonic in('mov','lea') and i.op_str.startswith(('r8d,','r9d,','r8,','r8b','r9b')):
            last[i.op_str.split(',')[0]]=i.op_str.split(',',1)[1].strip()
        if i.mnemonic=='call':
            try: t=int(i.op_str,16)-base
            except:
                out.append('  '*depth+f'{i.address-base:x} VCALL {i.op_str}'); continue
            if t in RD:
                n=RD[t]
                if n=='READ': out.append('  '*depth+f'{i.address-base:x} READ {last.get("r8d","?")}/{last.get("r9d","?")}')
                elif n=='XFORM': out.append('  '*depth+f'{i.address-base:x} XFORM {last.get("r8d","?")}')
                else: out.append('  '*depth+f'{i.address-base:x} {n}')
            elif t in SKIP: pass
            elif depth<maxd:
                sub=psum(t,depth+1,maxd)
                if any(any(k in s for k in ('READ','REFID','VSVAL','WSTR','XFORM')) for s in sub):
                    out.append('  '*depth+f'{i.address-base:x} call {t:#x}'); out+=sub
        if i.mnemonic.startswith('cmp') and 'al,' in i.op_str: out.append('  '*depth+f'{i.address-base:x} {i.mnemonic} {i.op_str}')
    return out
if __name__=='__main__':
    for l in psum(int(sys.argv[1],0), 0, int(sys.argv[2]) if len(sys.argv)>2 else 3): print(l)
