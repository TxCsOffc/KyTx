"""Join replay iterations to the existing production scheduler records.

Native joins optionally use analyze-gpuview.py's packet export and its raw-QPC
origin. GPU elapsed brackets and native residence are not pure execution time.
"""
import argparse
import csv
import json
import statistics
from pathlib import Path


def read_csv(path):
    with Path(path).open(newline='', encoding='utf-8-sig') as stream:
        return list(csv.DictReader(stream))


def analyze(replay_csv, profile, packets=None, qpc_zero=None, qpc_frequency=10_000_000):
    replay_csv, profile = Path(replay_csv), Path(profile)
    metadata = json.loads(Path(str(replay_csv) + '.meta.json').read_text(encoding='utf-8-sig'))
    iterations = read_csv(str(replay_csv) + '.iterations.csv')
    submits, scopes, cpu = [], {}, []
    for meta in profile.parent.glob(profile.name + '*.meta.csv'):
        if read_csv(meta)[0]['role'] != 'replay':
            continue
        source = Path(str(meta)[:-len('.meta.csv')])
        submits.extend(dict(r, scheduler=str(source)) for r in read_csv(str(source) + '.submits.csv'))
        scopes[str(source)] = read_csv(source)
        cpu.extend(read_csv(str(source) + '.cpu.csv'))
    drops = sum(int(r['timer_drops']) + int(r['event_drops']) for r in cpu)
    if packets is not None and qpc_zero is None:
        raise ValueError('Native joins require a verified raw-QPC origin, not a UTC estimate')
    native = json.loads(Path(packets).read_text()) if packets else None
    phase_index = {(r['context'], r['sequence']): r for r in native['queue_phases']} if native else {}
    joins = []
    for row in iterations:
        begin, end = int(row['flush_begin_ns']), int(row['flush_end_ns'])
        matches = [s for s in submits if s['thread'] == row['thread'] and s['tick'] == row['tick']
                   and begin <= int(s['queue_enter_ns']) <= int(s['submit_end_ns']) <= end]
        join = dict(row, submit_match_count=len(matches))
        if len(matches) != 1:
            joins.append(join)
            continue
        submit = matches[0]
        join['submit'] = submit
        gpu = [g for g in scopes[submit['scheduler']] if g['tick'] == row['tick']]
        join['gpu_scopes'] = gpu
        join['split_scopes'] = sum(g['split'] != '0' for g in gpu)
        if native:
            # MSVC steady_clock is QPC-derived. Preserve the verified ETW QPC
            # origin/frequency; raw GPU clocks remain uncalibrated to CPU.
            a = int(submit['queue_enter_ns']) / 1000 - qpc_zero * 1e6 / qpc_frequency
            b = int(submit['submit_end_ns']) / 1000 - qpc_zero * 1e6 / qpc_frequency
            pid = int(metadata['replay_pid'])
            candidates = [q for q in native['queues'] if q['type'] == 0 and
                          int(q['thread']) == int(row['thread']) and
                          ('(' + str(pid) + ')') in q['process'].replace(' ', '') and
                          a - 1 <= q['a'] <= b + 1 and not q.get('censored')]
            join['native_match_count'] = len(candidates)
            if len(candidates) == 1:
                queue = candidates[0]
                join['native_queue'] = queue
                join['native_phase'] = phase_index.get((queue['context'], queue['sequence']))
        joins.append(join)
    groups = {}
    for row in joins:
        if row['warmup'] == '1':
            continue
        groups.setdefault(row['capture'], []).append(row)
    summary = {name: {
        'iterations': len(rows),
        'gpu_median_ms': statistics.median(float(r['gpu_ms']) for r in rows),
        'wall_median_ms': statistics.median(float(r['wall_ms']) for r in rows),
        'submit_matches': sum(r['submit_match_count'] == 1 for r in rows),
        'native_matches': sum(r.get('native_match_count') == 1 for r in rows),
    } for name, rows in groups.items()}
    return {'metadata': metadata, 'iterations': len(joins), 'event_and_query_drops': drops,
            'submit_matches': sum(j['submit_match_count'] == 1 for j in joins),
            'split_scopes': sum(j.get('split_scopes', 0) for j in joins),
            'qpc_zero_ticks': qpc_zero, 'qpc_frequency': qpc_frequency,
            'summary': summary, 'joins': joins,
            'limits': 'Elapsed GPU stage brackets/native residence; not pure execution. No GPU/CPU clock calibration.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('replay_csv')
    parser.add_argument('profile')
    parser.add_argument('--packets')
    parser.add_argument('--qpc-zero-ticks', type=int)
    parser.add_argument('--qpc-frequency', type=int, default=10_000_000)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    result = analyze(args.replay_csv, args.profile, args.packets, args.qpc_zero_ticks, args.qpc_frequency)
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('joins', 'metadata')}, indent=2))
    if result['submit_matches'] != result['iterations'] or result['event_and_query_drops'] or result['split_scopes']:
        raise SystemExit('Incomplete submit/timestamp coverage; inspect saved joins')
