import sys;sys.path.insert(0,'.')
from trcmp import *
want=loadcpp('ct_og3b.bin'); fv,cfs=load('dump/og3.bin'); a=A(fv); prev=None; k=0
for ref,t,v,fl,fid,b in cfs:
    if t!=1 or ref not in want: continue
    k+=1
    try: p=a.parse(ref,fl,b); ok=p==len(b)
    except (Unsupported,EOFError): prev=ref; continue
    except Exception as ex:
        if "EXCEPTION" not in str(ex): prev=ref; continue
        print(k,'first fault',hex(ref),'after',hex(prev) if prev else None, ex, hex(a.c.e.r(UC_X86_REG_RIP)-BASE), backtrace(a.c.e,6)); break
    prev=ref
