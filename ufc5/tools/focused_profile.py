"""Additional attribution for the existing production event store; no second recorder."""
import bisect
import csv
import math
import json
import statistics
from collections import Counter, defaultdict

WATCH = (0x1164b80000, 0x1140008000, 0x1167f00000, 0x1165e00000)
MS = 1_000_000


def num(r, k):
    x = r.get(k, 0)
    return x if isinstance(x, int) else int(x, 0) if x else 0


def interval(r):
    return num(r, 'begin_ns'), num(r, 'end_ns')


def merged(rows):
    result = []
    for a, b in sorted(rows):
        if b <= a:
            continue
        if result and a <= result[-1][1]:
            result[-1] = result[-1][0], max(b, result[-1][1])
        else:
            result.append((a, b))
    return result


class Ranges:
    def __init__(self, rows):
        self.rows = merged(rows)
        self.ends = [b for a, b in self.rows]

    def clipped(self, a, b):
        i = bisect.bisect_right(self.ends, a)
        result = []
        while i < len(self.rows):
            lo, hi = self.rows[i]
            if lo >= b:
                break
            result.append((max(a, lo), min(b, hi)))
            i += 1
        return result


def write(path, rows):
    with path.open('w', encoding='utf-8', newline='') as f:
        if rows:
            w = csv.DictWriter(f, list(rows[0]))
            w.writeheader()
            w.writerows(rows)


def percentile(values, q):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * q) - 1)] if values else 0


PHASE = {'draw_state_cpu': 'state', 'cpu_prepare_draw_state': 'state',
         'draw_bindings_cpu': 'bindings', 'cpu_prepare_graphics_bindings': 'bindings',
         'draw_commit_cpu': 'commit', 'draw_targets_cpu': 'targets',
         'draw_pipeline_cpu': 'pipeline', 'draw_vertex_index_cpu': 'vertex_index'}


def fine_calls(rows, exclusions):
    """Subtract only direct child scopes and the union of recorded waits/diagnostics.

    Per-thread RAII intervals form a tree. Identical boundaries put phase wrappers
    first; partially overlapping intervals are not treated as child intervals.
    """
    stack, result = [], []
    rows = sorted(rows, key=lambda r: (num(r, 'begin_ns'), -num(r, 'end_ns'),
                                      0 if r['kind'].startswith('draw_') else 1))
    nodes = []
    for r in rows:
        a, b = interval(r)
        if b <= a:
            continue
        while stack and not (stack[-1]['a'] <= a and b <= stack[-1]['b']):
            stack.pop()
        phase = PHASE.get(r['kind'], stack[-1]['phase'] if stack else 'outside_draw')
        n = dict(row=r, a=a, b=b, children=[], phase=phase)
        if stack:
            stack[-1]['children'].append((a, b))
        nodes.append(n)
        stack.append(n)
    for n in nodes:
        r, a, b = n['row'], n['a'], n['b']
        skipped = exclusions.clipped(a, b)
        excluded = sum(hi-lo for lo, hi in skipped)
        self_excluded = sum(hi-lo for lo, hi in merged(skipped + n['children']))
        result.append(dict(frame=num(r, 'frame'), thread=num(r, 'thread'), kind=r['kind'],
                           phase=n['phase'], key=r.get('resource', ''), parameter=r.get('bytes', ''),
                           begin_ns=a, end_ns=b, inclusive_net_ms=(b-a-excluded)/MS,
                           self_net_ms=(b-a-self_excluded)/MS, site=f"{r.get('file','')}:{r.get('line','')}",
                           context=r.get('context', '')))
    return result


def overlaps(e, addr, size):
    a, s = num(e, 'address'), num(e, 'bytes')
    return s > 0 and a < addr + size and addr < a + s


def describe(e, shader_index):
    if not e:
        return ''
    shaders = shader_index.get((num(e, '_scheduler'), num(e, 'begin_ns'), num(e, 'address')), [])
    shader = ';'.join(f"{s.get('resource','')}@guest_submit={s.get('bytes','')}" for s in shaders)
    return (f"{e['kind']} address={e.get('address','')} bytes={e.get('bytes','')} "
            f"buffer={e.get('resource','')} tick={e.get('tick','')} shader={shader} "
            f"context={e.get('context','')}")


def lifetime_rows(events, gpu, submits, owner):
    es = list(events.query("_scheduler=? AND (kind LIKE 'readback_%' OR kind LIKE 'gpu_writer_%' "
                           "OR kind LIKE 'gpu_reader_%' OR kind LIKE 'cpu_consume_%' "
                           "OR kind LIKE 'cpu_writer_%' OR kind='cpu_invalidate_intent' "
                           "OR kind LIKE 'role_color_%' OR kind='buffer_use_shader' "
                           "OR kind='buffer_readback_sync' OR kind='timeline_wait')", (owner,)))
    tx = defaultdict(list)
    shader_index = defaultdict(list)
    for e in es:
        if num(e, 'transaction'):
            tx[num(e, 'transaction')].append(e)
        if e['kind'] == 'buffer_use_shader':
            shader_index[(owner, num(e, 'begin_ns'), num(e, 'address'))].append(e)
    uses = sorted((e for e in es if e['kind'].startswith(('gpu_writer_', 'gpu_reader_', 'cpu_consume_',
                                                       'cpu_writer_')) or e['kind'] == 'cpu_invalidate_intent'),
                  key=lambda e: num(e, 'begin_ns'))
    times = [num(e, 'begin_ns') for e in uses]
    roles = [e for e in es if e['kind'].startswith('role_color_')]
    gpu_tx, batch = defaultdict(list), defaultdict(list)
    for g in gpu:
        if num(g, '_scheduler') == owner:
            gpu_tx[num(g, 'transaction')].append(g)
    for s in submits:
        batch[(num(s, '_scheduler'), num(s, 'tick'))].append(s)
    previous_buffer = {}
    result = []
    readbacks = sorted((e for e in es if e['kind'] == 'buffer_readback_sync'), key=lambda e: num(e, 'begin_ns'))
    for r in readbacks:
        records = tx[num(r, 'transaction')]
        requests = [e for e in records if e['kind'] in {'readback_read_request', 'readback_write_fault'}]
        windows = [e for e in records if e['kind'] == 'readback_widened_window']
        # Legacy traces have no request identities; retain their older CSV separately.
        if not requests or not windows:
            continue
        req, window = requests[0], windows[0]
        addr, size = num(window, 'address'), num(window, 'bytes')
        targets = [a for a in WATCH if a < addr + size and addr < a + 512*1024]
        if not targets:
            continue
        lo, hi = interval(r)
        waits = [e for e in records if e['kind'] == 'timeline_wait' and lo <= num(e,'begin_ns') < hi
                 and num(e,'thread') == num(r,'thread')]
        published = [e for e in records if e['kind'] == 'readback_published' and lo <= num(e,'begin_ns') <= hi]
        content = [e for e in records if e['kind'].startswith('readback_content_') and lo <= num(e,'begin_ns') <= hi]
        pub_end = max((num(e, 'begin_ns') for e in published), default=hi)
        before = bisect.bisect_left(times, num(req, 'begin_ns'))
        writers = [e for e in reversed(uses[:before]) if e['kind'].startswith('gpu_writer_') and overlaps(e, addr, size)]
        known = next((e for e in writers if e['kind'] != 'gpu_writer_address_unknown'), None)
        unknown = next((e for e in writers if e['kind'] == 'gpu_writer_address_unknown'), None)
        next_use = next((e for e in uses[bisect.bisect_left(times, pub_end):]
                         if overlaps(e, addr, size)), None)
        next_cpu = next((e for e in uses[bisect.bisect_left(times, pub_end):]
                         if e['kind'].startswith('cpu_consume_') and overlaps(e, addr, size)), None)
        origins = [e for e in records if e['kind'] == 'readback_request_origin']
        copies = [g for g in gpu_tx[num(r,'transaction')] if g['kind']=='copy_target_readback'
                  and not num(g,'split')]
        bs = [s for w in waits for s in batch[(owner,num(w,'bytes'))]]
        handle = window.get('resource', '')
        old = previous_buffer.get(addr)
        previous_buffer[addr] = handle
        result.append(dict(targets=';'.join(hex(a) for a in targets), frame=num(r,'frame'),
                           transaction=num(r,'transaction'), guest_request_address=req.get('resource',''),
                           requested_bytes=num(req,'bytes'), request_type=req['kind'],
                           request_context=req.get('context',''), request_site=f"{req.get('file','')}:{req.get('line','')}",
                           request_function=req.get('function',''),
                           requester_thread=origins[0].get('resource','') if origins else '', producer_thread=num(r,'thread'),
                           request_dispatch_elapsed_ms=sum((num(e,'end_ns')-num(e,'begin_ns'))/MS for e in origins),
                           drain_address=hex(addr), drain_window_bytes=size, packed_bytes=num(r,'bytes'), buffer=handle,
                           buffer_changed='first' if old is None else str(old != handle),
                           readback_enter_ns=lo, readback_exit_ns=hi,
                           wait_ticks=';'.join(str(num(e,'bytes')) for e in waits), wait_calls=len(waits),
                           wait_begin_ns=';'.join(str(num(e,'begin_ns')) for e in waits),
                           wait_end_ns=';'.join(str(num(e,'end_ns')) for e in waits),
                           wait_union_ms=sum(b-a for a,b in merged(interval(e) for e in waits))/MS,
                           wait_max_ms=max(((num(e,'end_ns')-num(e,'begin_ns'))/MS for e in waits),default=0),
                           batch_submit_ns=';'.join(str(num(s,'queue_enter_ns')) for s in bs),
                           copy_elapsed_ms=sum(float(g['gpu_ms']) for g in copies), copy_query_count=len(copies),
                           copy_start_raw=';'.join(g['start_raw'] for g in copies), copy_end_raw=';'.join(g['end_raw'] for g in copies),
                           publication_ns=';'.join(str(num(e,'begin_ns')) for e in published),
                           publication_spans=';'.join(f"{e.get('address','')}+{e.get('bytes','')}" for e in published),
                           exact_content_results=';'.join(e['kind'].removeprefix('readback_content_') for e in content),
                           content_fingerprints=';'.join(e.get('resource','') for e in content),
                           content_spans=';'.join(f"{e.get('address','')}+{e.get('bytes','')}" for e in content),
                           last_known_gpu_writer=describe(known,shader_index), last_known_writer_ns=num(known or {},'begin_ns'),
                           last_opaque_gpu_writer=describe(unknown,shader_index),
                           last_opaque_writer_ns=num(unknown or {},'begin_ns'),
                           first_instrumented_cpu_consumer=describe(next_cpu,shader_index),
                           first_cpu_consumer_ns=num(next_cpu or {},'begin_ns'),
                           next_instrumented_use=describe(next_use,shader_index),
                           next_use_ns=num(next_use or {},'begin_ns'),
                           observed_roles=';'.join(sorted({e['kind'] for e in roles if overlaps(e,addr,size)}))))
    return result


def analyze_focused(events, gpu, submits, frames, owner, owner_thread, output, base=None):
    sampling_factor = 1
    if base and base.with_name(base.name+'.manifest.json').exists():
        sampling_factor = json.loads(base.with_name(base.name+'.manifest.json').read_text(encoding='utf-8-sig')).get('fine_cpu_root_every',1)
    eligible = {r['frame'] for r in frames[1:-1] if not r['event_drops']}
    if len(frames) == 1:
        eligible = {frames[0]['frame']}  # synthetic single-epoch fixture / explicitly partial window
    scoped = list(events.query("_scheduler=? AND thread=? AND (kind LIKE 'cpu_%' OR kind LIKE 'draw_%') AND end_ns>begin_ns",
                               (owner, owner_thread)))
    detailed = {num(e,'frame') for e in scoped if e['kind'].startswith('cpu_')} & eligible
    from_kinds = {'timeline_wait','queue_wait_idle','priority_callback_wait','priority_drain_wait','draw_commit_wait'}
    # Both schedulers share this process's CPU clock. A parent can enter the other
    # scheduler on the same foreground thread; its waits/probes still exclude work.
    excluded = list(events.query("thread=? AND (kind LIKE 'profile_%' OR kind IN (" +
                                  ','.join('?' for _ in from_kinds) + '))', (owner_thread, *from_kinds)))
    emissions = list(events.query("thread=? AND probe_end_ns>end_ns AND end_ns>0", (owner_thread,)))
    excluded_ranges = Ranges([interval(e) for e in excluded] +
                             [(num(e,'end_ns'),num(e,'probe_end_ns')) for e in emissions])
    calls = []
    by_frame = defaultdict(list)
    for e in scoped:
        if num(e,'frame') in detailed:
            by_frame[num(e,'frame')].append(e)
    for es in by_frame.values():
        calls += fine_calls(es, excluded_ranges)
    write(output/'cpu-fine-calls.csv',calls)
    groups = defaultdict(list)
    repeat = defaultdict(list)
    for r in calls:
        groups[(r['phase'],r['kind'])].append(r)
        if r['kind'].startswith('cpu_'):
            repeat[(r['frame'],r['phase'],r['kind'],r['key'],r['parameter'])].append(r)
    summary=[]
    for (phase,kind), rs in groups.items():
        self_epoch=Counter()
        for r in rs:
            self_epoch[r['frame']]+=r['self_net_ms']
        inc=[r['inclusive_net_ms'] for r in rs]
        factor = sampling_factor if kind.startswith('cpu_') else 1
        summary.append(dict(phase=phase,kind=kind,calls=len(rs),total_inclusive_net_ms=sum(inc),
                            total_self_net_ms=sum(r['self_net_ms'] for r in rs),
                            median_call_ms=statistics.median(inc),p95_call_ms=percentile(inc,.95),max_call_ms=max(inc),
                            sampled_epochs=len(detailed),median_self_per_sampled_epoch_ms=statistics.median(self_epoch[f] for f in detailed),
                            sampling_factor=factor,estimated_self_per_epoch_ms=statistics.median(self_epoch[f] for f in detailed)*factor))
    coarse=Counter()
    estimated=Counter()
    for r in summary:
        if r['kind'].startswith('draw_'):
            coarse[r['phase']]+=r['total_inclusive_net_ms']
        else:
            estimated[r['phase']]+=r['total_self_net_ms']*sampling_factor
    closure=[]
    for phase,total in coarse.items():
        ratio=estimated[phase]/total if total else 0
        closure.append(dict(phase=phase,measured_phase_total_ms=total,scaled_fine_self_total_ms=estimated[phase],
                             ratio=ratio,withhold_scaled_budget=ratio>1.25))
    write(output/'cpu-sampling-closure.csv',closure)
    invalid={r['phase'] for r in closure if r['withhold_scaled_budget']}
    for r in summary:
        r['scaled_budget_usable']=r['phase'] in coarse and r['phase'] not in invalid and r['kind'].startswith('cpu_')
    # Legacy draw-phase wrappers include unsampled work; they must not compete
    # with sampled finer functions as if both represented complete child trees.
    summary.sort(key=lambda r:(not r['kind'].startswith('cpu_'),-r['total_self_net_ms']))
    write(output/'cpu-fine-summary.csv',summary)
    epoch_groups=defaultdict(list)
    for r in calls:
        if r['kind'].startswith('cpu_'):
            epoch_groups[(r['frame'],r['phase'],r['kind'])].append(r)
    write(output/'cpu-fine-per-epoch.csv',[
        dict(frame=f,phase=p,kind=k,recorded_calls=len(rs),
             recorded_self_ms=sum(r['self_net_ms'] for r in rs),
             raw_scaled_self_ms=sum(r['self_net_ms'] for r in rs)*sampling_factor,
             scaled_budget_usable=p in coarse and p not in invalid,
             recorded_inclusive_ms=sum(r['inclusive_net_ms'] for r in rs),
             median_call_ms=statistics.median(r['inclusive_net_ms'] for r in rs),
             p95_call_ms=percentile([r['inclusive_net_ms'] for r in rs],.95),
             max_call_ms=max(r['inclusive_net_ms'] for r in rs))
        for (f,p,k),rs in sorted(epoch_groups.items())])
    repeated=[dict(frame=k[0],phase=k[1],kind=k[2],key=k[3],parameter=k[4],calls=len(rs),
                   inclusive_net_ms=sum(r['inclusive_net_ms'] for r in rs),self_net_ms=sum(r['self_net_ms'] for r in rs),
                   identity_scope='observed argument/fingerprint subset; not a complete validity key')
              for k,rs in repeat.items() if len(rs)>1]
    repeated.sort(key=lambda r:r['self_net_ms'],reverse=True)
    write(output/'cpu-repetitions.csv',repeated)
    counters=Counter()
    parameters=Counter()
    for e in events.query("_scheduler=? AND (kind LIKE 'cache_%' OR kind LIKE 'binding_%' OR kind='state_rebuilt')",(owner,)):
        f,k=num(e,'frame'),e['kind']
        if f in detailed:
            counters[(f,k)]+=1
            parameters[(f,k)]+=num(e,'bytes')
    write(output/'cpu-fine-counters.csv',[dict(frame=f,kind=k,calls=n,
          binding_resource_count=parameters[(f,k)] if k=='binding_rebuilt' else '',
          state_color_attachment_count=parameters[(f,k)] if k=='state_rebuilt' else '')
          for (f,k),n in sorted(counters.items())])
    lifetimes=lifetime_rows(events,gpu,submits,owner)
    write(output/'readback-lifetimes.csv',lifetimes)
    addresses=[]
    for a in WATCH:
        rs=[r for r in lifetimes if hex(a) in r['targets'].split(';')]
        if not rs:
            continue
        count=Counter(v for r in rs for v in r['exact_content_results'].split(';') if v)
        waits=[r['wait_union_ms'] for r in rs]
        addresses.append(dict(address=hex(a),calls=len(rs),epochs=len({r['frame'] for r in rs}),
                              average_packed_bytes=statistics.mean(r['packed_bytes'] for r in rs),
                              total_wait_ms=sum(waits),average_wait_ms=statistics.mean(waits),max_wait_ms=max(r['wait_max_ms'] for r in rs),
                              total_copy_elapsed_ms=sum(r['copy_elapsed_ms'] for r in rs),copy_queries=sum(r['copy_query_count'] for r in rs),
                              exact_first_segments=count['first'],exact_equal_segments=count['equal'],exact_changed_segments=count['changed'],
                              untracked_segments=count['untracked'],buffer_changes=sum(r['buffer_changed']=='True' for r in rs),
                              request_contexts=';'.join(sorted({r['request_context'] for r in rs})),
                              observed_roles=';'.join(sorted({r['observed_roles'] for r in rs if r['observed_roles']}))))
    write(output/'readback-addresses.csv',addresses)
    lines=['# Targeted readback / CPU preparation profile','',f'CPU detail: {len(detailed)} epochs without CPU event drops: {sorted(detailed)}. Fine root selection probability: 1/{sampling_factor}.',
           '', '## Attribution rules','',
           '- CPU self time excludes direct instrumented children, recorded waits, and separately recorded diagnostic work. Remaining self time can contain uninstrumented children.',
           '- Per-call times still contain probe overhead; the probe cost cannot be completely subtracted. Epoch sampling is not a full-window census.',
           '- Fine totals are recorded sample totals. Estimated epoch contributions multiply them by the inverse root selection probability; these are sampling estimates, not exact budgets. Legacy phase self time includes unsampled child work.',
           '- Scaled fine totals are checked against measured phase envelopes. When they exceed an envelope by over 25%, scaled budgets are withheld: finite sampling of outliers and child-probe emission cost in parent self time can inflate them. Raw observations remain available.',
           '- Keys/fingerprints identify observed argument subsets. Repetition alone proves neither immutable state nor safe reuse.',
           '- GPU copy scopes are transfer-stage elapsed intervals, not pure copy engine execution. Their clock is not calibrated to the CPU clock.',
           '- Declared shader writers overapproximate actual writes. Opaque address writers and guest CPU writes prevent proving an absence of intervening writes.',
           '- Exact content equality compares already downloaded bytes with the previous same-address, same-size segment. Different segment sizes are separate comparison chains.',
           '- An instrumented consumer is not guaranteed to be the first actual consumer; a resumed guest faulting instruction may remain unidentified.',
           '', '## CPU self-time ranking','', '| Phase | Function | Recorded calls | Recorded self ms | Estimated self / epoch ms | p95 inclusive call ms |',
           '|---|---|---:|---:|---:|---:|']
    for r in summary[:30]:
        estimate=f"{r['estimated_self_per_epoch_ms']:.3f}" if r['scaled_budget_usable'] else 'withheld'
        lines.append(f"| {r['phase']} | {r['kind']} | {r['calls']} | {r['total_self_net_ms']:.3f} | {estimate} | {r['p95_call_ms']:.4f} |")
    lines+=['','## Readback addresses','','| Address | Calls | Average bytes | Total wait ms | Average wait ms | Max wait ms | Copy elapsed total ms | Equal / changed segments | Context |',
            '|---|---:|---:|---:|---:|---:|---:|---|---|']
    for r in addresses:
        lines.append(f"| {r['address']} | {r['calls']} | {r['average_packed_bytes']:.0f} | {r['total_wait_ms']:.3f} | {r['average_wait_ms']:.3f} | {r['max_wait_ms']:.3f} | {r['total_copy_elapsed_ms']:.3f} | {r['exact_equal_segments']} / {r['exact_changed_segments']} | {r['request_contexts']} |")
    lines+=['','Details: cpu-fine-calls.csv, cpu-fine-summary.csv, cpu-fine-per-epoch.csv, cpu-fine-counters.csv, cpu-repetitions.csv, readback-lifetimes.csv, readback-addresses.csv.',
            'Missing lifetime rows mean the targeted instrumentation was off, addresses moved, or no matching requested readback was observed. Do not infer zero cost.']
    (output/'focused-report.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
