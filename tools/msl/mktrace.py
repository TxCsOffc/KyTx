import re,sys
h,gx,gy,tx,ty=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5])
src=open(f'/tmp/msl_direct/mh_{h}.metal').read()
types=dict((m.group(2),m.group(1)) for m in re.finditer(r'^\t(\w+) (v\d+) = \(\w+\)0;$',src,re.M))
out=[];slots=[];n=0
for ln in src.split('\n'):
    out.append(ln)
    m=re.match(r'^(\t+)(v\d+) = (.*);$',ln)
    if m and m.group(2) in types and n<16000:
        t=types[m.group(2)]
        nm=m.group(2)
        if t in ('uint4','uint2','uint3'):
            w=int(t[-1]); n0=n+1
            stmts=' '.join(f'kyty_dbg[{n0+i}] = {nm}[{i}];' for i in range(w))
            n+=w
            out.append(f'{m.group(1)}if (kyty_t) {{ {stmts} }}')
            slots.append((n0,nm,t,ln.strip()[:150]))
            continue
        conv={'float':f'as_type<uint>({nm})','uint':nm,'bool':f'(uint){nm}','half':f'(uint)as_type<ushort>({nm})'}.get(t)
        if conv is None: continue
        n+=1
        out.append(f'{m.group(1)}if (kyty_t) {{ kyty_dbg[{n}] = {conv}; }}')
        slots.append((n,nm,t,ln.strip()[:150]))
res='\n'.join(out)
sig='uint liid [[thread_index_in_threadgroup]])'
assert sig in res
res=res.replace(sig,'uint liid [[thread_index_in_threadgroup]], device uint* kyty_dbg [[buffer(26)]])',1)
res=res.replace('(void)ab; (void)pc_data; (void)sizes;',f'(void)ab; (void)pc_data; (void)sizes;\n\tconst bool kyty_t = (tgid.x == {gx}u && tgid.y == {gy}u && tid.x == {tx}u && tid.y == {ty}u);',1)
open(f'/tmp/fix6/trace_{h}.metal','w').write(res)
open(f'/tmp/fix6/trace_{h}.map','w').write('\n'.join(f'{a}\t{b}\t{c}\t{d}' for a,b,c,d in slots))
print(len(slots))
