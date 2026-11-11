"""Separate CPU-valid and GPU-complete cohorts in an existing production analysis."""
import argparse
import csv
import importlib.util
import json
import sqlite3
import statistics
from pathlib import Path

spec = importlib.util.spec_from_file_location('production_profile', Path(__file__).with_name('analyze-production-profile.py'))
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def read(path):
    with path.open(newline='', encoding='utf-8-sig') as handle:
        return [r for r in csv.DictReader(handle) if None not in r and all(v is not None for v in r.values())]


def summarize(trace):
    analysis = Path(str(trace) + '.analysis')
    frames = read(analysis / 'per-frame.csv')
    cpu = read(Path(str(trace) + '.cpu.csv'))
    interior = frames[1:-1]
    complete = [r for r in interior if not any(int(r[k]) for k in ('timer_drops', 'event_drops', 'uncollected_batches'))]
    valid_cpu = [r for r in interior if not int(r['event_drops'])]
    keys = ('wall_ms', 'foreground_explicit_wait_ms', 'guest_cpu_excluding_wait_submit_profile_ms',
            'translation_other_cpu_ms', 'recording_excluding_wait_submit_profile_ms', 'submit_cpu_ms',
            'profile_flush_cpu_ms', 'profile_collect_cpu_ms', 'all_captured_batches_complete_gap_lower_bound_ms',
            'gpu_batch_elapsed_union_ms', 'graphics_scope_union_ms', 'guest_compute_scope_union_ms',
            'detile_dispatch_scope_union_ms', 'copy_clear_scope_union_ms', 'queue_submits',
            'average_submit_gap_ms', 'max_submit_gap_ms', 'foreground_timeline_waits')
    def cohort(rows, gpu_complete=False):
        return dict(epochs=[int(r['frame']) for r in rows], count=len(rows), medians={
            k: statistics.median(float(r[k]) for r in rows if r[k])
            for k in keys if any(r[k] for r in rows) and (gpu_complete or k not in (
                'gpu_batch_elapsed_union_ms', 'graphics_scope_union_ms', 'guest_compute_scope_union_ms',
                'detile_dispatch_scope_union_ms', 'copy_clear_scope_union_ms'))})
    out = {'trace': str(trace), 'cpu_valid_interior': cohort(valid_cpu), 'gpu_complete_interior': cohort(complete, True),
           'coverage': {k: sum(int(r[k]) for r in frames) for k in ('timer_drops', 'event_drops', 'split_scopes', 'uncollected_batches')},
           'limits': 'Use CPU metrics from CPU-valid epochs and GPU metrics from GPU-complete epochs. '
                     'Dropped timers may correlate with long GPU backlog, making the complete subset unrepresentative. '
                     'GPU elapsed is not active execution; CPU/GPU clocks and independent medians are not additive.'}
    boundaries = {int(r['frame']): (int(r['begin_ns']), int(r['end_ns'])) for r in cpu}
    lo, hi = min(a for a, b in boundaries.values()), max(b for a, b in boundaries.values())
    db = sqlite3.connect(analysis / 'events.sqlite')
    db.row_factory = sqlite3.Row
    owner = db.execute("SELECT thread, count(*) n FROM events WHERE kind='guest_process_slice' GROUP BY thread ORDER BY n DESC LIMIT 1").fetchone()['thread']
    # Only foreground waits and draw phases; reuse the analyzer's interval unions.
    phases = list(db.execute("SELECT kind,begin_ns,end_ns FROM events WHERE thread=? AND kind LIKE 'draw_%'", (owner,)))
    waits = list(db.execute('SELECT begin_ns,end_ns FROM events WHERE thread=? AND kind IN (' +
                           ','.join('?' for _ in profile.WAIT_KINDS) + ')', (owner, *profile.WAIT_KINDS)))
    out['foreground_thread'] = owner
    out['draw_phases_net_waits'] = {}
    for kind in sorted({r['kind'] for r in phases}):
        values = []
        for frame in valid_cpu:
            begin, end = boundaries[int(frame['frame'])]
            spans = [(max(begin, r['begin_ns']), min(end, r['end_ns'])) for r in phases
                     if r['kind'] == kind and r['begin_ns'] < end and r['end_ns'] > begin]
            if not spans:
                continue
            waiting = [(max(begin, r['begin_ns']), min(end, r['end_ns'])) for r in waits
                       if r['begin_ns'] < end and r['end_ns'] > begin]
            values.append(profile.duration(spans) - profile.duration(profile.overlap(spans, waiting)))
        if values:
            out['draw_phases_net_waits'][kind] = {'epochs': len(values), 'median_ms': statistics.median(values)}
    db.close()
    by_address = {}
    for r in read(analysis / 'readbacks.csv'):
        if int(r['thread']) != owner:
            continue
        item = by_address.setdefault(r['guest_address'], {'calls': 0, 'host_wait_ms': 0, 'inclusive_ms': 0, 'packed_bytes': 0})
        item['calls'] += 1
        item['host_wait_ms'] += float(r['host_wait_union_ms'])
        item['inclusive_ms'] += float(r['cpu_inclusive_ms'])
        item['packed_bytes'] += int(r['packed_bytes'])
    total_wait = sum(v['host_wait_ms'] for v in by_address.values())
    for v in by_address.values():
        v['wait_percent'] = v['host_wait_ms'] * 100 / total_wait if total_wait else 0
    out['readback_addresses'] = by_address
    out['producer_wall_seconds'] = sum(float(r['wall_ms']) for r in frames) / 1000
    out['present_api_calls'] = sum(int(r['present_calls']) for r in frames)
    out['median_draws_computes'] = {k: statistics.median(int(r[k]) for r in cpu[1:-1]) for k in ('draws', 'computes')}
    out['host_begin_ns'], out['host_end_ns'] = lo, hi
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    args = parser.parse_args()
    result = summarize(args.trace)
    Path(str(args.trace) + '.budget.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
