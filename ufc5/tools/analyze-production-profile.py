"""Analyze bounded production traces without equating elapsed scopes with active GPU time."""
import argparse
import csv
import statistics
import sqlite3
import re
import json
from collections import Counter, defaultdict
from pathlib import Path
from focused_profile import analyze_focused
from materializer_profile import analyze_materializer
from readback_writer_profile import analyze_writers

NS_MS = 1_000_000
WAIT_KINDS = {'timeline_wait', 'queue_wait_idle', 'priority_callback_wait',
              'priority_drain_wait', 'acquire_next_image_wait', 'draw_commit_wait',
              'video_flip_done_wait', 'guest_pm4_blocked_wait', 'guest_blocked_wait',
              'guest_suspend_point_wait', 'guest_idle_completion_wait'}


def number(row, key, default=0):
    value = row.get(key)
    return value if isinstance(value, int) else int(value, 0) if value else default


def union(intervals):
    result = []
    for a, b in sorted((a, b) for a, b in intervals if b > a):
        if result and a <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], b))
        else:
            result.append((a, b))
    return result


def duration(intervals):
    return sum(b - a for a, b in union(intervals)) / NS_MS


def overlap(left, right):
    a, b = union(left), union(right)
    i = j = 0
    result = []
    while i < len(a) and j < len(b):
        lo, hi = max(a[i][0], b[j][0]), min(a[i][1], b[j][1])
        if hi > lo:
            result.append((lo, hi))
        if a[i][1] <= b[j][1]:
            i += 1
        else:
            j += 1
    return result


def read_csv(path, scheduler, role):
    if not path.exists():
        return []
    with path.open(encoding='utf-8-sig', newline='') as f:
        rows = []
        for row in csv.DictReader(f):
            # A snapshot of a running trace can end in a partially flushed row.
            if None in row or any(v is None for v in row.values()):
                continue
            row['_scheduler'], row['_role'] = scheduler, role
            rows.append(row)
        return rows


def write_csv(path, rows):
    with path.open('w', encoding='utf-8', newline='') as f:
        if rows:
            writer = csv.DictWriter(f, list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


class EventStore:
    """Keep verbose bounded-window events on disk; do not require several GB of Python dictionaries."""
    columns = ('kind','frame','begin_ns','end_ns','thread','tick','transaction','address','resource','bytes',
               'src_stage','dst_stage','src_access','dst_access','old_layout','new_layout','src_queue','dst_queue',
               'guest_queue','file','line','function','context','probe_end_ns','_scheduler','_role')
    integers = {'frame','begin_ns','end_ns','probe_end_ns','thread','tick','transaction','line','_scheduler'}

    def __init__(self, path, sources):
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.execute('PRAGMA journal_mode=OFF')
        self.db.execute('PRAGMA synchronous=OFF')
        self.fingerprint = json.dumps([(str(p), p.stat().st_size, p.stat().st_mtime_ns) for p in sources if p.exists()])
        self.reused = False
        try:
            cached = self.db.execute('SELECT fingerprint FROM event_cache').fetchone()
            self.reused = bool(cached and cached[0] == self.fingerprint)
        except sqlite3.OperationalError:
            pass
        if self.reused:
            existing = {r[1] for r in self.db.execute('PRAGMA table_info(events)')}
            for k in ('function', 'context'):
                if k not in existing:
                    self.db.execute(f'ALTER TABLE events ADD COLUMN "{k}" TEXT DEFAULT \'\'')
            if 'probe_end_ns' not in existing:
                self.db.execute('ALTER TABLE events ADD COLUMN probe_end_ns INTEGER DEFAULT 0')
            return
        self.db.execute('DROP TABLE IF EXISTS event_cache')
        self.db.execute('DROP TABLE IF EXISTS events')
        self.db.execute('CREATE TABLE events (' + ','.join('"' + k + '"' + (' INTEGER' if k in self.integers else ' TEXT')
                                                         for k in self.columns) + ')')

    def add(self, path, sid, role):
        if self.reused or not path.exists():
            return
        def values():
            with path.open(encoding='utf-8-sig', newline='') as f:
                for r in csv.DictReader(f):
                    if None in r or any(v is None for v in r.values()):
                        continue
                    r['_scheduler'], r['_role'] = sid, role
                    yield tuple(int(r.get(k) or 0) if k in self.integers else r.get(k, '') for k in self.columns)
        names = ','.join('"' + k + '"' for k in self.columns)
        self.db.executemany('INSERT INTO events (' + names + ') VALUES (' + ','.join('?' for _ in self.columns) + ')', values())
        self.db.commit()

    def index(self):
        if self.reused:
            return
        for column in ('begin_ns','end_ns','frame','kind','transaction'):
            self.db.execute(f'CREATE INDEX event_{column} ON events ("{column}")')
        self.db.commit()
        self.db.execute('CREATE TABLE event_cache (fingerprint TEXT)')
        self.db.execute('INSERT INTO event_cache VALUES (?)', (self.fingerprint,))
        self.db.commit()

    def query(self, where='1', params=()):
        return (dict(r) for r in self.db.execute('SELECT * FROM events WHERE ' + where, params))

    def kinds(self, names):
        names = tuple(names)
        return self.query('kind IN (' + ','.join('?' for _ in names) + ')', names)

    def __iter__(self):
        return self.query()


def gpu_intervals(rows):
    return [(r['_a'], r['_b']) for r in rows if not number(r, 'split')]


def analyze(base, output):
    paths = [base] + sorted(p for p in base.parent.glob(base.name + '.scheduler-*.csv')
                           if re.fullmatch(re.escape(base.name) + r'\.scheduler-\d+\.csv', p.name))
    output.mkdir(parents=True, exist_ok=True)
    gpu, cpu, submits = [], [], []
    events = EventStore(output / 'events.sqlite', [Path(str(p) + '.events.csv') for p in paths])
    identities = []
    for sid, path in enumerate(paths):
        meta = read_csv(Path(str(path) + '.meta.csv'), sid, '')
        role = meta[0]['role'] if meta else 'unknown'
        identities += meta
        for target, suffix in ((gpu, ''), (cpu, '.cpu.csv'),
                               (submits, '.submits.csv')):
            target.extend(read_csv(Path(str(path) + suffix), sid, role))
        events.add(Path(str(path) + '.events.csv'), sid, role)
        print(f'Loaded scheduler {sid} ({role})', flush=True)
    events.index()
    gpu = [r for r in gpu if r.get('start_raw') and r['kind'] != 'control']
    if not gpu or not cpu:
        events.db.close()
        raise ValueError('No completed GPU queries / CPU epochs. Arm a capture and let it complete first.')
    anchor = number(gpu[0], 'start_raw')
    for r in gpu:
        mask = number(r, 'timestamp_mask')
        period = float(r['timestamp_period_ns'])
        offset = (number(r, 'start_raw') - anchor) & mask
        if offset > (mask + 1) // 2:
            offset -= mask + 1
        span = (number(r, 'end_raw') - number(r, 'start_raw')) & mask
        r['_a'], r['_b'] = offset * period, (offset + span) * period
    primary = [r for r in cpu if r['_role'] == 'producer']
    if not primary:
        primary = [r for r in cpu if r['_role'] == 'replay']
    owner = primary[0]['_scheduler'] if primary else cpu[0]['_scheduler']
    primary = [r for r in cpu if r['_scheduler'] == owner]
    queue_by_scheduler = {r['_scheduler']: r['queue_handle'] for r in identities}
    queue_cpu = defaultdict(list)
    for s in submits:
        queue_cpu[s['queue_handle']].append(s)
    observed_idle = []
    completed_at = defaultdict(list)
    for e in events.kinds({'gpu_batch_complete_observed'}):
        if e['kind'] == 'gpu_batch_complete_observed':
            completed_at[(e['_scheduler'], number(e, 'resource'))].append(number(e, 'end_ns'))
    for sequence in queue_cpu.values():
        sequence.sort(key=lambda r: number(r, 'queue_enter_ns'))
        for current, following in zip(sequence, sequence[1:]):
            observations = [t for t in completed_at[(current['_scheduler'], number(current, 'tick'))]
                            if t >= number(current, 'submit_end_ns') and t < number(following, 'submit_begin_ns')]
            if observations:
                observed_idle.append((min(observations), number(following, 'submit_begin_ns')))
    # The command processor thread is identified by its recorded guest slices.
    thread_counts = Counter(number(r, 'thread') for r in events.kinds({'guest_process_slice'}))
    if not thread_counts:
        thread_counts = Counter(number(r, 'thread') for r in submits if r['_scheduler'] == owner)
    owner_thread = thread_counts.most_common(1)[0][0] if thread_counts else 0
    frame_gpu, frame_submits = defaultdict(list), defaultdict(list)
    for r in gpu:
        frame_gpu[number(r, 'frame')].append(r)
    for r in submits:
        frame_submits[number(r, 'frame')].append(r)
    frames = []
    for c in primary:
        frame = number(c, 'frame')
        lo, hi = number(c, 'begin_ns'), number(c, 'end_ns')
        ev = list(events.query('begin_ns < ? AND end_ns > ?', (hi, lo)))
        fg = [r for r in ev if number(r, 'thread') == owner_thread]
        def cpu_ranges(rows):
            return [(max(lo, number(r, 'begin_ns')), min(hi, number(r, 'end_ns'))) for r in rows]
        waits = cpu_ranges([r for r in fg if r['kind'] in WAIT_KINDS])
        guest = cpu_ranges([r for r in fg if r['kind'] in {'guest_process_slice', 'guest_callback_cpu'}])
        detailed = any(r['kind'].startswith('draw_') for r in fg)
        recording = cpu_ranges([r for r in fg if r['kind'].startswith('draw_') or
                                r['kind'] in {'detile_record_cpu', 'tile_record_cpu'}])
        gs, ss = frame_gpu[frame], frame_submits[frame]
        submit_intervals = [(number(r, 'submit_begin_ns'), number(r, 'submit_end_ns')) for r in submits
                            if number(r, 'thread') == owner_thread]
        diagnostic_intervals = cpu_ranges([r for r in fg if r['kind'].startswith('profile_')])
        emission_rows = list(events.query('thread=? AND end_ns<? AND probe_end_ns>? AND probe_end_ns>end_ns AND end_ns>0', (owner_thread, hi, lo)))
        emission_intervals = [(max(lo,number(r,'end_ns')),min(hi,number(r,'probe_end_ns'))) for r in emission_rows]
        diagnostic_intervals += emission_intervals
        excluded_guest = waits + submit_intervals + diagnostic_intervals
        # Every queue submit contains one command buffer in the current scheduler.
        starts = sorted(number(r, 'queue_enter_ns') for r in ss)
        gaps = [(b - a) / NS_MS for a, b in zip(starts, starts[1:])]
        batches = [r for r in gs if r['kind'] == 'submit']
        classified = [r for r in gs if r['kind'] in {'render', 'compute', 'detile_dispatch',
                                                    'tile_dispatch', 'detile_clear'} or r['kind'].startswith('copy_')]
        barrier_rows = list(events.query("frame=? AND kind LIKE 'barrier_%'", (frame,)))
        broad = [r for r in barrier_rows if (number(r, 'src_stage') | number(r, 'dst_stage')) & 0x10000]
        missing = sum(not any(g['_scheduler'] == s['_scheduler'] and number(g, 'tick') == number(s, 'tick')
                              for g in batches) for s in ss if number(s, 'timer_page') != 0xffffffff)
        rows = {
            'frame': frame, 'wall_ms': float(c['wall_ms']),
            'present_calls': sum(r['kind'] == 'queue_present_cpu' and lo <= number(r, 'begin_ns') < hi for r in ev),
            'cpu_draw_detail_available': detailed,
            'draw_phase_aggregate_inclusive_ms': sum(float(c[k]) for k in ('draw_state_ms','draw_bindings_ms','draw_vertex_index_ms','draw_pipeline_ms','draw_targets_ms','draw_commit_ms')),
            'guest_process_inclusive_ms': duration(guest),
            'guest_process_excluding_waits_ms': duration(guest) - duration(overlap(guest, waits)),
            'guest_cpu_excluding_wait_submit_profile_ms': duration(guest) - duration(overlap(guest, excluded_guest)),
            'translation_other_cpu_ms': duration(guest) - duration(overlap(guest, excluded_guest + recording)) if detailed or not number(c, 'draws') else None,
            'recording_scopes_inclusive_ms': duration(recording) if detailed or not number(c, 'draws') else None,
            'recording_excluding_waits_ms': duration(recording) - duration(overlap(recording, waits)) if detailed or not number(c, 'draws') else None,
            'recording_excluding_wait_submit_profile_ms': duration(recording) - duration(overlap(recording, excluded_guest)) if detailed or not number(c, 'draws') else None,
            'foreground_explicit_wait_ms': duration(waits),
            'submit_cpu_ms': sum((number(r, 'submit_end_ns') - number(r, 'submit_begin_ns')) / NS_MS for r in ss),
            'submit_pre_api_ms': sum((number(r, 'queue_enter_ns') - number(r, 'submit_begin_ns')) / NS_MS for r in ss),
            'build_elapsed_ms': sum((number(r, 'submit_begin_ns') - number(r, 'build_begin_ns')) / NS_MS
                                    for r in ss if number(r, 'build_begin_ns')),
            'profile_flush_cpu_ms': duration(cpu_ranges([r for r in ev if r['kind'] == 'profile_flush_cpu'])),
            'profile_collect_cpu_ms': duration(cpu_ranges([r for r in ev if r['kind'] == 'profile_collect_cpu'])),
            'profile_record_emission_cpu_ms': duration(emission_intervals),
            'guest_no_work_wait_ms': duration(cpu_ranges([r for r in fg if r['kind'] == 'guest_no_work_wait'])),
            'all_captured_batches_complete_gap_lower_bound_ms': duration(overlap([(lo, hi)], observed_idle)),
            'gpu_batch_elapsed_union_ms': duration(gpu_intervals(batches)),
            'graphics_scope_union_ms': duration(gpu_intervals([r for r in gs if r['kind'] == 'render'])),
            'guest_compute_scope_union_ms': duration(gpu_intervals([r for r in gs if r['kind'] == 'compute'])),
            'detile_dispatch_scope_union_ms': duration(gpu_intervals([r for r in gs if r['kind'] == 'detile_dispatch'])),
            'copy_clear_scope_union_ms': duration(gpu_intervals([r for r in gs if r['kind'].startswith('copy_') or r['kind'] == 'detile_clear'])),
            'unclassified_batch_elapsed_ms': max(0, duration(gpu_intervals(batches)) - duration(overlap(gpu_intervals(batches), gpu_intervals(classified)))),
            'queue_submits': len(ss), 'command_buffers': len(ss),
            'graphics_only_submits': sum(number(r, 'draws') > 0 and number(r, 'guest_computes') == 0 for r in ss),
            'compute_only_submits': sum(number(r, 'draws') == 0 and number(r, 'guest_computes') > 0 for r in ss),
            'mixed_submits': sum(number(r, 'draws') > 0 and number(r, 'guest_computes') > 0 for r in ss),
            'other_submits': sum(number(r, 'draws') == 0 and number(r, 'guest_computes') == 0 for r in ss),
            'wait_semaphore_dependencies': sum(number(r, 'wait_dependencies') for r in ss),
            'average_submit_gap_ms': statistics.mean(gaps) if gaps else 0,
            'max_submit_gap_ms': max(gaps, default=0),
            'own_scheduler_known_complete_gap_ms': sum(max(0, number(r, 'submit_begin_ns') - number(r, 'known_idle_since_ns')) / NS_MS
                                                      for r in ss if number(r, 'known_idle_since_ns')),
            'foreground_timeline_waits': sum(r['kind'] == 'timeline_wait' for r in fg),
            'queue_idle_calls': sum(r['kind'] == 'queue_wait_idle' for r in ev),
            'barrier_resource_records': len(barrier_rows), 'broad_barrier_resource_records': len(broad),
            'detile_calls': sum(r['kind'] == 'detile' for r in gs),
            'timer_drops': sum(number(r, 'timer_drops') for r in cpu if number(r, 'frame') == frame),
            'event_drops': sum(number(r, 'event_drops') for r in cpu if number(r, 'frame') == frame),
            'split_scopes': sum(number(r, 'split') for r in gs), 'uncollected_batches': missing,
        }
        frames.append(rows)
    output.mkdir(parents=True, exist_ok=True)
    write_csv(output / 'per-frame.csv', frames)
    transactions = defaultdict(lambda: {'gpu': [], 'cpu': []})
    for source, kind in ((gpu, 'gpu'), (events.query('"transaction">0 AND kind NOT LIKE \'barrier_%\''), 'cpu')):
        for r in source:
            if number(r, 'transaction'):
                transactions[(r['_scheduler'], number(r, 'transaction'))][kind].append(r)
    txrows = []
    submit_by_tick, wait_by_tick = defaultdict(list), defaultdict(list)
    for r in submits:
        submit_by_tick[(r['_scheduler'], number(r, 'tick'))].append(r)
    for r in events.kinds({'timeline_wait'}):
        wait_by_tick[(r['_scheduler'], number(r, 'bytes'))].append(r)
    for (sid, txid), records in transactions.items():
        gs, es = records['gpu'], records['cpu']
        detiles = [r for r in gs if r['kind'] == 'detile_dispatch' and not number(r, 'split')]
        if not detiles:
            continue
        def endpoint(kind, end=False, last=False):
            found = [r['_b' if end else '_a'] for r in gs if r['kind'] == kind and not number(r, 'split')]
            return (max(found) if last else min(found)) if found else None
        t0 = endpoint('detile_pre')
        t1 = min(r['_a'] for r in detiles)
        t2 = max(r['_b'] for r in detiles)
        t3 = endpoint('resource_ready', end=True, last=True)
        if t3 is None:
            t3 = endpoint('detile_post', end=True, last=True)
        consumers = [r['_a'] for r in gs if r['kind'].startswith('consumer_') and not number(r, 'split')]
        t4 = min(consumers) if consumers else None
        scopes = [r for r in es if r['kind'] == 'detile_transaction']
        outer = [r for r in es if r['kind'] == 'upload_transaction'] or scopes
        recording = [r for r in es if r['kind'] == 'detile_record_cpu']
        ticks = {number(r, 'tick') for r in detiles}
        batch_submits = [r for tick in sorted(ticks) for r in submit_by_tick[(sid, tick)]]
        host_waits = [r for tick in sorted(ticks) for r in wait_by_tick[(sid, tick)]]
        allocations = [r for r in es if r['kind'] == 'allocation_memory_properties']
        txrows.append({'scheduler': sid, 'transaction': txid, 'frame': number(detiles[0], 'frame'),
                       'guest_address': detiles[0]['address'], 'detile_dispatch_groups': len(detiles),
                       'cpu_detile_inclusive_ms': duration([(number(r, 'begin_ns'), number(r, 'end_ns')) for r in scopes]),
                       'C0_function_enter_ns': min((number(r, 'begin_ns') for r in outer), default=''),
                       'C1_record_begin_ns': min((number(r, 'begin_ns') for r in recording), default=''),
                       'C2_batch_submit_ns': ';'.join(str(number(r, 'queue_enter_ns')) for r in batch_submits),
                       'C3_host_wait_begin_ns': ';'.join(str(number(r, 'begin_ns')) for r in host_waits),
                       'C4_host_wait_end_ns': ';'.join(str(number(r, 'end_ns')) for r in host_waits),
                       'C3_C4_host_wait_threads': ';'.join(str(r['thread']) for r in host_waits),
                       'C5_function_exit_ns': max((number(r, 'end_ns') for r in outer), default=''),
                       'source_memory_properties': ';'.join(r['resource'] for r in allocations),
                       'source_memory_type_index': ';'.join(r['bytes'] for r in allocations),
                       'T0_gpu_relative_ns': t0, 'T1_gpu_relative_ns': t1, 'T2_gpu_relative_ns': t2,
                       'T3_gpu_relative_ns': t3, 'T4_gpu_relative_ns': t4,
                       'pre_dependency_clear_elapsed_ms': (t1 - t0) / NS_MS if t0 is not None else '',
                       'dispatch_elapsed_union_ms': duration(gpu_intervals(detiles)),
                       'dispatch_end_to_ready_ms': (t3 - t2) / NS_MS if t3 is not None else '',
                       'ready_to_first_consumer_ms': (t4 - t3) / NS_MS if t4 is not None and t3 is not None else '',
                       'first_consumer_traced': bool(consumers),
                       'batch_ticks': ';'.join(map(str, sorted({number(r, 'tick') for r in gs})))})
    write_csv(output / 'transactions.csv', txrows)
    readbacks = []
    for (sid, txid), records in transactions.items():
        es = records['cpu']
        for r in es:
            if r['kind'] not in {'buffer_readback_sync', 'buffer_readback_async'}:
                continue
            lo, hi = number(r, 'begin_ns'), number(r, 'end_ns')
            waits = [e for e in es if e['kind'] in WAIT_KINDS and e['thread'] == r['thread']
                     and lo <= number(e, 'begin_ns') and number(e, 'end_ns') <= hi]
            sources = [e for e in es if e['kind'] == 'readback_source_allocation'
                       and lo <= number(e, 'begin_ns') <= hi]
            metadata = [e for e in es if e['kind'] == 'allocation_memory_properties'
                        and any(e['begin_ns'] == s['begin_ns'] for s in sources)]
            readbacks.append({'scheduler': sid, 'transaction': txid, 'frame': r['frame'],
                              'kind': r['kind'], 'thread': r['thread'],
                              'guest_address': r['resource'], 'packed_bytes': r['bytes'],
                              'begin_ns': lo, 'end_ns': hi, 'cpu_inclusive_ms': (hi - lo) / NS_MS,
                              'host_wait_union_ms': duration([(number(e, 'begin_ns'), number(e, 'end_ns')) for e in waits]),
                              'host_wait_calls': len(waits),
                              'wait_batch_ticks': ';'.join(str(number(e, 'bytes')) for e in waits if e['kind'] == 'timeline_wait'),
                              'source_buffer_handles': ';'.join(e['resource'] for e in sources),
                              'source_memory_properties': ';'.join(e['resource'] for e in metadata),
                              'source_memory_type_index': ';'.join(e['bytes'] for e in metadata)})
    write_csv(output / 'readbacks.csv', readbacks)
    waits_by_site = defaultdict(list)
    for r in events.kinds(WAIT_KINDS):
        if r['kind'] in WAIT_KINDS:
            waits_by_site[(r['kind'], r['thread'], r['resource'], r['file'], r['line'])].append(r)
    waitrows = []
    for key, rows in waits_by_site.items():
        kind, thread, resource, file, line = key
        waitrows.append({'kind': kind, 'thread': thread, 'resource': resource, 'file': file, 'line': line,
                         'calls': len(rows), 'elapsed_union_ms': duration([(number(r, 'begin_ns'), number(r, 'end_ns')) for r in rows]),
                         'max_call_ms': max((number(r, 'end_ns') - number(r, 'begin_ns')) / NS_MS for r in rows),
                         'foreground': int(thread) == owner_thread})
    waitrows.sort(key=lambda r: r['elapsed_union_ms'], reverse=True)
    write_csv(output / 'wait-sites.csv', waitrows)
    barrier_counts = Counter((r['kind'], r['file'], r['line'], r['src_stage'], r['dst_stage'],
                              r['src_access'], r['dst_access'], r['old_layout'], r['new_layout'],
                              r['src_queue'], r['dst_queue']) for r in events.query("kind LIKE 'barrier_%'"))
    barrierrows = [dict(zip(('kind','file','line','src_stage','dst_stage','src_access','dst_access',
                            'old_layout','new_layout','src_queue','dst_queue'), key),
                        resource_records=count, broad=bool((int(key[3]) | int(key[4])) & 0x10000),
                        all_commands_to_all_commands=bool(int(key[3]) & int(key[4]) & 0x10000),
                        ownership_transfer=key[9] != key[10] and int(key[9]) != 0xffffffff and int(key[10]) != 0xffffffff)
                   for key, count in barrier_counts.most_common()]
    write_csv(output / 'barrier-sites.csv', barrierrows)
    interior = frames[1:-1] if len(frames) > 2 else frames
    good = [r for r in interior if not any(r[k] for k in ('timer_drops','event_drops','uncollected_batches'))]
    sample = good or interior
    scope_groups = defaultdict(list)
    for g in gpu:
        if g['kind'] in {'compute', 'detile_dispatch', 'tile_dispatch', 'render'} or g['kind'].startswith('copy_'):
            scope_groups[(g['kind'], g['shader'])].append(g)
    scopes = []
    for (kind, payload), rows in scope_groups.items():
        by_frame = defaultdict(list)
        for r in rows:
            by_frame[number(r, 'frame')].append(r)
        scopes.append({'kind': kind, 'shader_hash_or_payload': payload, 'calls': len(rows),
                       'elapsed_union_ms': duration(gpu_intervals(rows)),
                       'median_per_epoch_ms': statistics.median(duration(gpu_intervals(by_frame[r['frame']])) for r in sample),
                       'max_scope_ms': max(float(r['gpu_ms']) for r in rows),
                       'split_scopes': sum(number(r, 'split') for r in rows)})
    scopes.sort(key=lambda r: r['median_per_epoch_ms'], reverse=True)
    write_csv(output / 'gpu-scopes.csv', scopes)
    # Only the same native queue's submit envelopes can be compared as a coverage signal.
    queues = defaultdict(list)
    for r in gpu:
        if r['kind'] == 'submit' and not number(r, 'split'):
            queues[queue_by_scheduler.get(r['_scheduler'], str(r['_scheduler']))].append((r['_a'], r['_b']))
    queue_gaps = {q: [(b[0] - a[1]) / NS_MS for a, b in zip(union(v), union(v)[1:])] for q, v in queues.items()}
    lines = ['# Production graphics profile', '', f'Trace: `{base}`', '',
             f'Captured {len(frames)} producer epochs; {len(gpu)} GPU ranges; {len(submits)} queue submits. Foreground thread: {owner_thread}.', '',
             '## Measurement rules', '',
             '- Epoch numbers are guest SuspendPoint enqueue counter hints, not guaranteed presented frame IDs.',
             '- GPU ranges are pipeline-stage elapsed time, including stalls/preemption. They are not shader active time.',
             '- CPU and GPU use different clocks; no calibrated clock correlation is available. Their totals cannot be added.',
             '- Scoped categories overlap. Union lengths avoid repeated scopes within each category; categories still must not be summed.',
             '- Build elapsed includes translator work, host waits, and time when a buffer is open but not being recorded.',
             '- Broad barrier counts count resource entries, not calls. They identify conservatism, not its measured latency.',
             '- Known-complete gaps concern one scheduler. Uncovered queue timestamp gaps are not proof of device idleness or its cause.',
             '- First/last epochs are excluded from aggregate results. Missing queries, drops, and split scopes remain visible.', '',
             '## Measured contributors', '', '| Metric | Median / epoch (ms) |', '|---|---:|']
    metrics = ('wall_ms', 'foreground_explicit_wait_ms', 'guest_cpu_excluding_wait_submit_profile_ms', 'translation_other_cpu_ms',
               'recording_excluding_waits_ms', 'submit_cpu_ms', 'profile_flush_cpu_ms',
               'profile_collect_cpu_ms', 'profile_record_emission_cpu_ms', 'recording_excluding_wait_submit_profile_ms',
               'guest_no_work_wait_ms', 'all_captured_batches_complete_gap_lower_bound_ms',
               'gpu_batch_elapsed_union_ms', 'graphics_scope_union_ms', 'guest_compute_scope_union_ms',
               'detile_dispatch_scope_union_ms', 'copy_clear_scope_union_ms', 'unclassified_batch_elapsed_ms')
    for key in metrics:
        values = [r[key] for r in sample if r[key] is not None]
        lines.append(f'| {key} | {statistics.median(values):.3f} |' if values else f'| {key} | unavailable |')
    lines += ['', f"Per-draw CPU intervals available in {sum(r['cpu_draw_detail_available'] for r in sample)} of {len(sample)} selected epochs; inclusive phase counters remain collected in every epoch.",
              f"Captured present API calls: {sum(r['present_calls'] for r in frames)}; producer window wall: {sum(r['wall_ms'] for r in frames)/1000:.3f} s. Do not treat epochs as display frames."]
    ranked = sorted(((statistics.median(r[key] for r in sample), key) for key in
                     ('foreground_explicit_wait_ms', 'guest_cpu_excluding_wait_submit_profile_ms', 'submit_cpu_ms')), reverse=True)
    lines += ['', '### Foreground CPU ranking (scope attribution)', '']
    for rank, (value, key) in enumerate(ranked, 1):
        lines.append(f'{rank}. {key}: {value:.3f} ms median. This is measured elapsed attribution, not yet a causal optimization result.')
    lines += ['', '### GPU scope ranking (elapsed, includes possible stalls)', '',
              '| Kind | Shader hash / payload | Calls | Median / epoch (ms) | Max scope (ms) |',
              '|---|---|---:|---:|---:|']
    for r in scopes[:12]:
        lines.append(f"| {r['kind']} | {r['shader_hash_or_payload']} | {r['calls']} | {r['median_per_epoch_ms']:.3f} | {r['max_scope_ms']:.3f} |")
    lines += ['', '### Wait call sites', '', '| Kind | Thread | Calls | Total elapsed (ms) | Site |', '|---|---:|---:|---:|---|']
    for r in waitrows[:12]:
        lines.append(f"| {r['kind']} | {r['thread']} | {r['calls']} | {r['elapsed_union_ms']:.3f} | {Path(r['file']).name}:{r['line']} |")
    lines += ['', '## Coverage and submission gaps', '']
    lines.append(f"Timer drops: {sum(r['timer_drops'] for r in frames)}; event drops: {sum(r['event_drops'] for r in frames)}; split scopes: {sum(r['split_scopes'] for r in frames)}; uncollected batches: {sum(r['uncollected_batches'] for r in frames)}.")
    lines.append(f'Complete interior epochs used: {len(good)} / {len(interior)}. If none are complete, these aggregates are partial observations.')
    lines.append('The completed-batch gap lower bound requires a host completion observation for the latest submitted buffer across both captured schedulers, followed by a later CPU submit. It proves no captured command buffer remained outstanding during that interval; it does not claim the entire device was idle or assign the cause. Window edges and untraced external processes are excluded from that claim.')
    for q, gaps in queue_gaps.items():
        lines.append(f'Queue {q}: {len(gaps)} uncovered timestamp gaps; total {sum(gaps):.3f} ms; maximum {max(gaps, default=0):.3f} ms across the window.')
    lines += ['', '## Detile transactions', '',
              f'{len(txrows)} traced transactions, {sum(r["first_consumer_traced"] for r in txrows)} with a first consumer marker.',
              'T0→T1 includes dependency eligibility and clear; T1→T2 is compute-stage elapsed dispatch scope; T2→T3 includes post barriers/copies; T3→T4 is time to a traced consumer stage. Neither barrier cost nor pure shader execution is isolated by subtracting overlapping pipeline-stage timestamps.', '',
              '## Interpretation limits', '',
              'These files identify measured waits, submission cadence, and GPU scope inflation. They do not by themselves distinguish hardware preemption from dependency stalls inside a compute scope. Use a GPU execution trace if the measured scope inflation remains unexplained. Gameplay claims require a capture taken in the paused fight, not a replay or boot/menu trace.', '',
              'The earlier 14–32 ms contested replay measurements were from a separate replay process running alongside the game; they are not direct production detile timings.', '',
              'Files: per-frame.csv, transactions.csv, readbacks.csv, wait-sites.csv, barrier-sites.csv. Raw traces retain resources, masks, layouts, thread IDs, and scheduler-local ticks.']
    (output / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    analyze_focused(events, gpu, submits, frames, owner, owner_thread, output, base)
    analyze_writers(events, submits, output, frames, gpu)
    analyze_materializer(base.with_name(base.name+'.materializer.csv'), output)
    events.db.close()
    return frames


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or Path(str(args.trace) + '.analysis')
    try:
        frames = analyze(args.trace, output)
    except (ValueError, OSError) as exc:
        parser.exit(1, str(exc) + '\n')
    print(f'{len(frames)} epochs analyzed: {output / "report.md"}')
