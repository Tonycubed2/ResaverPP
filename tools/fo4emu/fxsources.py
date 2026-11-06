# fxsources.py <plugins.json> <fxdump.jsonl>: the EffectSource of every (item, effect index) the dump's actors have, for
# fxremove.cpp. plugins.json: per plugin file name {"mgef": {"0x..": [flags, archetype, actor value, edid]}, "items":
# {"0x..": [type, edid, [[mgef, magnitude, duration]...], spell type, cast type, poison]}} (local form ids as the
# plugin has them; the DLCs' own records have top byte 01, their single master Fallout4.esm 00).
import json,sys
MG=json.load(open(sys.argv[1]))
L=[json.loads(l) for l in open(sys.argv[2])]
pl=L[0]['plugins']
seen=set()
for a in L[1:]:
    for e in a['fx']:
        fid,idx=e[0],e[1]
        if (fid,idx) in seen: continue
        seen.add((fid,idx))
        p=fid>>24
        if p>=len(pl) or pl[p] not in MG: continue
        name=pl[p]; own=0 if name=='Fallout4.esm' else 1
        it=MG[name]['items'].get(hex((own<<24)|(fid&0xffffff)))
        if not it or idx>=len(it[2]): continue
        ef=it[2][idx][0]
        m=MG['Fallout4.esm' if ef>>24==0 else name]['mgef'].get(hex(ef))
        if not m: continue
        av=m[2]
        if av and av>>24: av=(p<<24)|(av&0xffffff)   # a DLC's own AVIF: the save's index of that DLC
        print('%08X %d %X %d %X %s %d %d %d'%(fid,idx,m[0],m[1],av,it[0],it[3],it[4],int(it[5])))
