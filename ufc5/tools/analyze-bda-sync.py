"""Summarize bounded live BDA stages; epochs and presentation counters are distinct."""
import argparse
import csv
import json
import re
import statistics
from pathlib import Path


def aggregate(rows):
    counts = ('calls', 'reference_calls', 'candidate_calls', 'comparison_calls',
              'reference_visits', 'planned_visits', 'executed_candidate_visits',
              'full_passes', 'unchanged_passes', 'regions_considered', 'regions_skipped',
              'dirty_bytes', 'upload_runs', 'upload_bytes', 'full_checks',
              'matched_checks', 'raced_checks', 'mismatches')
    times = ('cpu_ms', 'reference_cpu_ms', 'candidate_cpu_ms', 'comparison_cpu_ms')
    out = {k: sum(int(r[k]) for r in rows) for k in counts}
    out.update({k: sum(float(r[k]) for r in rows) for k in times})
    out['rows'] = len(rows)
    out['us_per_call'] = out['cpu_ms'] * 1000 / out['calls'] if out['calls'] else None
    out['candidate_percent'] = 100 * out['candidate_calls'] / out['calls'] if out['calls'] else 0
    out['count_closure_ok'] = out['calls'] == sum(out[k] for k in counts[1:4])
    out['time_closure_ok'] = abs(out['cpu_ms'] - sum(out[k] for k in times[1:])) < .02
    out['checks_closure_ok'] = out['full_checks'] == sum(out[k] for k in ('matched_checks', 'raced_checks', 'mismatches'))
    out['rejected'] = any(int(r['rejected']) for r in rows)
    return out


def analyze(prefix, fps_log):
    stages = sorted((json.loads(p.read_text(encoding='utf-8-sig')) for p in
        Path(prefix).parent.glob(Path(prefix).name + '.bda-stage-*.json')),
        key=lambda s: s['start']['host_ns'])
    if not stages:
        raise ValueError('No bounded BDA stages found')
    source = Path(prefix + '.bda-sync.csv')
    if not source.exists():
        source = Path(prefix + '.csv.bda-sync.csv')
    with source.open(newline='', encoding='utf-8-sig') as handle:
        reader = csv.DictReader(handle)
        fields = reader.fieldnames
        rows = [r for r in reader if None not in r and all(r.values())]
    first, last = stages[0]['start']['host_ns'], stages[-1]['end']['host_ns']
    selected = [r for r in rows if first <= int(r['host_ns']) <= last]
    frozen = Path(prefix + '.bda-fight.csv')
    with frozen.open('w', newline='', encoding='utf-8') as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(selected)
    fps = Path(fps_log).read_bytes()
    pattern = re.compile(r'PresentRate: pressure=(\d+) frames=(\d+) seconds=([\d.]+) fps=([\d.]+)')
    result = {'source': str(source), 'frozen': str(frozen), 'all_selected': aggregate(selected), 'stages': [],
        'limits': 'Only fully contained >=1.5-second CSV windows enter timings. Checks include edge rows. '
                  'BDA elapsed includes preemption/recording/upload work; shadow runs reference uploads. '
                  'Multiple GPU threads have overlapping reporting windows. Host presents, title FPS and guest epochs differ.'}
    compiled_source = source.with_name(source.name.replace('.bda-sync.csv', '.compiled-srt.csv'))
    if compiled_source.exists():
        with compiled_source.open(newline='', encoding='utf-8-sig') as handle:
            reader = csv.DictReader(handle)
            compiled_fields = reader.fieldnames
            compiled = [r for r in reader if None not in r and all(r.values()) and first <= int(r['host_ns']) <= last]
        compiled_frozen = Path(prefix + '.bda-compiled-guard.csv')
        with compiled_frozen.open('w', newline='', encoding='utf-8') as handle:
            writer = csv.DictWriter(handle, fieldnames=compiled_fields)
            writer.writeheader()
            writer.writerows(compiled)
        totals = [r for r in compiled if r['record_kind'] == 'total']
        result['compiled_srt_guard'] = {'frozen': str(compiled_frozen),
            'modes': sorted({r['mode'] for r in totals}),
            'calls': sum(int(r['calls']) for r in totals),
            'candidate_calls': sum(int(r['candidate_calls']) for r in totals),
            'checks': sum(int(r['full_checks']) for r in totals),
            'mismatches': sum(int(r['full_mismatches']) for r in totals),
            'rejected': any(int(r['rejected']) for r in totals)}
    for stage in stages:
        start, end = stage['start'], stage['end']
        mode = start['mode']
        stable = [r for r in selected if r['mode'] == mode and float(r['window_ms']) >= 1500 and
            int(r['host_ns']) - float(r['window_ms']) * 1e6 >= start['host_ns'] and
            int(r['host_ns']) <= end['host_ns']]
        item = {'stage': stage['stage'], 'mode': mode,
                'duration_seconds': (end['host_ns'] - start['host_ns']) / 1e9,
                'compiled_srt_modes': sorted({b['compiled_srt_mode'] for b in [start, end, *stage['samples']] if b.get('compiled_srt_mode')}),
                'process_cpu_ms': end['cpu_ms'] - start['cpu_ms'], 'bda': aggregate(stable)}
        item['threads'] = [dict(thread=thread, **aggregate([r for r in stable if r['thread'] == thread]))
                           for thread in sorted({r['thread'] for r in stable})]
        text = fps[int(start['fps_bytes']):int(end['fps_bytes'])].decode('utf-8', errors='replace')
        reports = [r for r in pattern.findall(text)[1:] if r[0] == '0']
        item['present_reports_excluding_first'] = len(reports)
        if reports:
            item['host_presents_per_second'] = sum(int(r[1]) for r in reports) / sum(float(r[2]) for r in reports)
        item['title_fps_samples'] = [int(m[1]) for b in stage['samples'] if (m := re.search(r'fps:\s*(\d+)', b['title']))]
        if item['title_fps_samples']:
            item['title_fps_median'] = statistics.median(item['title_fps_samples'])
        result['stages'].append(item)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix')
    parser.add_argument('--fps-log', default=r'D:\PS5\fps-pressure-test.txt')
    args = parser.parse_args()
    result = analyze(args.prefix, args.fps_log)
    Path(args.prefix + '.bda-fight.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
