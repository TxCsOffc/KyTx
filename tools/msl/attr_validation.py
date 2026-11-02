import re,collections,sys
cur=None; cnt=collections.defaultdict(collections.Counter)
for line in open(sys.argv[1],errors='replace'):
    m=re.match(r'\[SyncOp\] begin (.*?) groups',line)
    if m: cur=m.group(1); continue
    if 'Invalid Metal usage' in line or 'Caused GPU' in line:
        k=re.sub(r'offset \d+','offset N',line.strip())
        k=k.replace("Invalid Metal usage: ","").replace(" in 'device' or 'constant' address space","").replace(', executing compute function: "main0"','')[:100]
        cnt[cur][k]+=1
for op,c in cnt.items():
    print(op)
    for k,v in c.most_common(4): print('   ',v,k)
