import sys,re
# condensed listing: reads with sizes, calls, branches, compares
lines=open(sys.argv[1]).read().splitlines()
lo=int(sys.argv[2],16) if len(sys.argv)>2 else 0; hi=int(sys.argv[3],16) if len(sys.argv)>3 else 1<<40
r8=None; r9=None; rdx=None
for l in lines:
    a,_,rest=l.partition(' ')
    ad=int(a,16)
    m,_,ops=rest.partition(' ')
    if m in('mov','lea') and ops.startswith('r8d,'): r8=ops[5:]
    if m in('mov','lea') and ops.startswith('r9d,'): r9=ops[5:]
    if m in('mov','lea') and ops.startswith('rdx,'): rdx=ops[5:]
    if m=='xor' and ops=='r9d, r9d': r9='0'
    if not(lo<=ad<hi): continue
    if '<READ>' in l:
        sz=r8 if r8!='r9d' else r9
        print(f'{a}   READ {sz} -> {rdx}')
    elif m=='call' or m.startswith('j') or m in('cmp','test','ret','ucomiss','comiss','bt','sete','setne') or '<' in l:
        print(f'{a} {m} {ops}')
