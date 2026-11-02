import struct,re,sys
h=sys.argv[1]; out=sys.argv[2]; roots=sys.argv[3].split(','); maxd=int(sys.argv[4])
m={}
for l in open(f'/tmp/fix6/trace_{h}.map'):
    a,b,c,d=l.rstrip('\n').split('\t'); m[int(a)]=(b,c,d)
sites={}
for a,(b,c,d) in m.items(): sites.setdefault(b,[]).append((a,c,d))
vals={}
for l in open(out):
    a,b=l.split(); vals[int(a)]=int(b,16)
def dec(t,v):
    if t=='float': return struct.unpack('<f',struct.pack('<I',v))[0]
    if t=='half': return struct.unpack('<e',struct.pack('<H',v&0xffff))[0]
    return v
def pick(n):
    s=sites.get(n)
    if not s: return None
    w=[x for x in s if x[0] in vals]
    return max(w) if w else s[-1]
seen=set()
def chain(n,depth=0):
    if n in seen or depth>maxd: return
    p=pick(n)
    if p is None: return
    seen.add(n)
    a,t,d=p
    if t in ('uint4','uint2','uint3') and a in vals:
        w=int(t[-1]); xs=[vals.get(a+i) for i in range(w)]
        txt=f"{n} {t} "+' '.join(('?' if x is None else '%08x/%g'%(x,struct.unpack('<f',struct.pack('<I',x))[0])) for x in xs)
    elif a in vals:
        v=dec(t,vals[a]); txt=f"{n} {t} {('%g'%v if isinstance(v,float) else hex(v))}"
    else: txt=f"{n} (nw)"
    print('  '*depth+txt+' | '+d[:110]+f'  [@{a}]')
    for dep in re.findall(r'\bv\d+\b',d.split('=',1)[1]):
        chain(dep,depth+1)
for r in roots: chain(r)
