"""Read the bounded materializer observer sidecar emitted by production rendering."""
import csv
from collections import defaultdict
from pathlib import Path


def write(path, rows):
    with path.open('w', newline='', encoding='utf-8') as f:
        if rows:
            writer=csv.DictWriter(f, list(rows[0]))
            writer.writeheader(); writer.writerows(rows)


def analyze_materializer(path, output):
    path, output=Path(path), Path(output)
    if not path.exists():
        return
    output.mkdir(parents=True, exist_ok=True)
    shaders=defaultdict(lambda: defaultdict(int))
    phases=defaultdict(lambda: defaultdict(int))
    ops=defaultdict(lambda: defaultdict(int))
    graphs=[]
    complete=False
    with path.open(newline='',encoding='utf-8-sig') as f:
        for r in csv.DictReader(f):
            if None in r or any(v is None for v in r.values()) or r['kind']=='kind':
                continue
            key=(r['thread'],r['shader'])
            if r['kind'] in {'aggregate','complete','shutdown','arm'} and int(r['calls']):
                a=shaders[key]
                for source, dest in [('calls','calls'),('samples','samples'),('cpu_ns','cpu_ns')]:
                    a[dest]+=int(r[source])
                metadata=dict(part.split('=',1) for part in r['metadata'].split(';') if '=' in part)
                a['sample_cpu_ns']+=int(metadata['sample_cpu_ns'])
            elif r['kind']=='phase':
                a=phases[(*key,r['phase'])]
                a['cpu_ns']+=int(r['cpu_ns']); a['samples']+=int(r['samples'])
            elif r['kind']=='opcode':
                a=ops[(*key,r['opcode'])]
                for field in ('calls','cpu_ns','words','hits','misses','failures'):
                    a[field]+=int(r[field])
            elif r['kind']=='graph':
                graphs.append(r)
            elif r['kind']=='marker' and r['metadata']=='complete':
                complete=True
    shader_rows=[]
    for (thread,shader), a in shaders.items():
        phase_ns=sum(v['cpu_ns'] for (t,s,p),v in phases.items() if (t,s)==(thread,shader))
        shader_rows.append(dict(thread=thread,shader=shader,calls=a['calls'],samples=a['samples'],
            cpu_total_ms=a['cpu_ns']/1e6,mean_call_us=a['cpu_ns']/max(1,a['calls'])/1e3,
            recorded_sample_cpu_ms=a['sample_cpu_ns']/1e6,
            recorded_phase_ms=phase_ns/1e6,
            recorded_setup_remainder_ms=(a['sample_cpu_ns']-phase_ns)/1e6,
            sample_closure_ok=phase_ns<=a['sample_cpu_ns']))
    shader_rows.sort(key=lambda r:r['cpu_total_ms'],reverse=True)
    phase_rows=[dict(thread=t,shader=s,phase=p,samples=a['samples'],recorded_cpu_ms=a['cpu_ns']/1e6,
                    mean_sample_call_us=a['cpu_ns']/max(1,a['samples'])/1e3)
                for (t,s,p),a in phases.items()]
    phase_rows.sort(key=lambda r:r['recorded_cpu_ms'],reverse=True)
    op_rows=[dict(thread=t,shader=s,opcode=op,visits=a['calls'],memo_hits=a['hits'],memo_misses=a['misses'],
                 failures=a['failures'],descriptor_words=a['words'],recorded_word_elapsed_ms=a['cpu_ns']/1e6,
                 mean_word_elapsed_us=a['cpu_ns']/max(1,a['words'])/1e3)
             for (t,s,op),a in ops.items()]
    op_rows.sort(key=lambda r:r['recorded_word_elapsed_ms'],reverse=True)
    write(output/'materializer-shaders.csv',shader_rows)
    write(output/'materializer-phases.csv',phase_rows)
    write(output/'materializer-opcodes.csv',op_rows)
    write(output/'descriptor-graphs.csv',graphs)
    lines=['# Sampled resource materialization','',f'Completion marker observed: {complete}.',
           '', 'Whole materializer elapsed is collected for every call in the armed window. Detailed phases and',
           'interpreter observations sample approximately 1/32 materializations. No extrapolated frame budget',
           'is produced. Timers/counters and host preemption remain in elapsed values. The sampled phase',
           'sum is checked against its matching sampled parent; setup/remainder includes unbracketed work.',
           '', 'Shader hashes aggregate static variants/stages with the same hash. Graph plan pointers distinguish',
           'live plans within this process, not persistent identities. Graph dependency counts are structural;',
           'opcode visits/memos/word counts are observed execution. Root-word elapsed includes its dependency',
           'tree and cannot be labeled pure opcode cost. Phase and word timings overlap; do not add them.',
           '', '| Shader | All calls | Samples | Total materializer ms | Mean call us | Sample setup/remainder ms |',
           '|---|---:|---:|---:|---:|---:|']
    for r in shader_rows[:20]:
        lines.append(f"| {r['shader']} | {r['calls']} | {r['samples']} | {r['cpu_total_ms']:.3f} | {r['mean_call_us']:.3f} | {r['recorded_setup_remainder_ms']:.3f} |")
    lines+=['','## Recorded phase ranking','','| Shader | Phase | Recorded ms | Mean sampled call us |',
            '|---|---|---:|---:|']
    for r in phase_rows[:20]:
        lines.append(f"| {r['shader']} | {r['phase']} | {r['recorded_cpu_ms']:.3f} | {r['mean_sample_call_us']:.3f} |")
    lines+=['','## Descriptor root expression ranking','','| Shader | Root | Words | Recorded elapsed ms | Mean word us |',
            '|---|---|---:|---:|---:|']
    for r in (r for r in op_rows[:40] if r['descriptor_words']):
        lines.append(f"| {r['shader']} | {r['opcode']} | {r['descriptor_words']} | {r['recorded_word_elapsed_ms']:.3f} | {r['mean_word_elapsed_us']:.3f} |")
    lines+=['','Detailed tables: materializer-shaders.csv, materializer-phases.csv, materializer-opcodes.csv, descriptor-graphs.csv.']
    (output/'materializer-report.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    analyze_materializer(args.csv,args.output or Path(str(args.csv)+'.analysis'))
