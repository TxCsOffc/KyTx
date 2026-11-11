"""Summarize bounded off/shadow/on measurements; keep replay timing separate."""
import argparse
import csv
import json
import re
import statistics
from collections import defaultdict
from pathlib import Path


def aggregate(rows):
    count_fields = ('calls', 'candidate_calls', 'reference_calls', 'comparison_calls',
                    'unsupported_calls', 'full_checks', 'full_mismatches')
    time_fields = ('cpu_ms', 'candidate_cpu_ms', 'reference_cpu_ms', 'comparison_cpu_ms',
                   'compile_ms', 'shadow_reference_ms', 'shadow_candidate_ms')
    result = {name: sum(int(r[name]) for r in rows) for name in count_fields}
    result.update({name: sum(float(r[name]) for r in rows) for name in time_fields})
    calls = result['calls']
    result['windows'] = len(rows)
    result['weighted_us_per_call'] = result['cpu_ms'] * 1000 / calls if calls else None
    result['candidate_percent'] = result['candidate_calls'] * 100 / calls if calls else 0
    result['count_closure_ok'] = calls == sum(result[k] for k in
        ('candidate_calls', 'reference_calls', 'comparison_calls'))
    result['elapsed_closure_ok'] = abs(result['cpu_ms'] - sum(result[k] for k in
        ('candidate_cpu_ms', 'reference_cpu_ms', 'comparison_cpu_ms'))) < .01
    result['rejected'] = any(int(r['rejected']) for r in rows)
    return result


def analyze(prefix, fps_log):
    boundaries = sorted((json.loads(p.read_text(encoding='utf-8-sig')) for p in
        Path(prefix).parent.glob(Path(prefix).name + '.compiled-stage-*.json')),
        key=lambda s: s['start']['host_ns'])
    if not boundaries:
        raise ValueError('No measurement stages found')
    source_path = Path(prefix + '.compiled-srt.csv')
    with source_path.open(newline='', encoding='utf-8-sig') as source:
        reader = csv.DictReader(source)
        fields = reader.fieldnames
        rows = [r for r in reader if None not in r and all(r.values())]
    first = boundaries[0]['start']['host_ns']
    last = boundaries[-1]['end']['host_ns']
    # Freeze completed rows; later emulator activity cannot alter this result.
    selected = [r for r in rows if first <= int(r['host_ns']) <= last]
    frozen = Path(prefix + '.compiled-fight.csv')
    with frozen.open('w', newline='', encoding='utf-8') as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(selected)
    fps = Path(fps_log).read_bytes()
    pattern = re.compile(r'PresentRate: pressure=(\d+) frames=(\d+) seconds=([\d.]+) fps=([\d.]+)')
    result = {'source': str(source_path), 'frozen': str(frozen), 'stages': [],
        'checks_all_selected_shader_rows': sum(int(r['full_checks']) for r in selected if r['record_kind']=='shader'),
        'mismatches_all_selected_shader_rows': sum(int(r['full_mismatches']) for r in selected if r['record_kind']=='shader'),
        'limits': 'Materializer elapsed includes preemption/read callbacks and controller overhead exists. '
                  'Shadow uses replay readers. Host presents and title FPS are distinct counters. '
                  'Only fully contained windows >=1.5s enter timing aggregates; checks include edge rows.'}
    for stage in boundaries:
        start, end = stage['start'], stage['end']
        mode = start['mode']
        stable = [r for r in selected if r['mode']==mode and
                  float(r['window_ms']) >= 1500 and
                  int(r['host_ns']) - float(r['window_ms'])*1e6 >= start['host_ns'] and
                  int(r['host_ns']) <= end['host_ns']]
        totals = [r for r in stable if r['record_kind']=='total']
        duration = (end['host_ns']-start['host_ns'])/1e9
        summary = {'stage': stage['stage'], 'mode': mode, 'duration_seconds': duration,
            'process_cpu_ms': end['cpu_ms']-start['cpu_ms'], 'materializer': aggregate(totals)}
        threads = defaultdict(list)
        shaders = defaultdict(list)
        for row in stable:
            (threads if row['record_kind']=='total' else shaders)[row['thread'] if row['record_kind']=='total' else row['shader']].append(row)
        summary['threads'] = []
        for thread, values in threads.items():
            item = aggregate(values)
            seconds = sum(float(r['window_ms']) for r in values)/1000
            item.update(thread=thread, sampled_seconds=seconds,
                        materializer_elapsed_ms_per_second=item['cpu_ms']/seconds)
            summary['threads'].append(item)
        summary['top_shaders'] = sorted((dict(shader=shader, **aggregate(values))
            for shader, values in shaders.items()), key=lambda r: r['cpu_ms'], reverse=True)[:15]
        text = fps[int(start['fps_bytes']):int(end['fps_bytes'])].decode('utf-8', errors='replace')
        reports = [r for r in pattern.findall(text)[1:] if r[0]=='0']
        summary['present_reports_excluding_first'] = len(reports)
        if reports:
            summary['present_rate_weighted'] = sum(int(r[1]) for r in reports)/sum(float(r[2]) for r in reports)
            summary['present_rate_range'] = [min(float(r[3]) for r in reports), max(float(r[3]) for r in reports)]
        title_fps = [int(m.group(1)) for sample in stage['samples']
                     if (m:=re.search(r'fps:\s*(\d+)', sample['title']))]
        summary['title_fps_samples'] = title_fps
        if title_fps:
            summary['title_fps_median'] = statistics.median(title_fps)
        result['stages'].append(summary)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix')
    parser.add_argument('--fps-log', default=r'D:\PS5\fps-pressure-test.txt')
    args = parser.parse_args()
    result = analyze(args.prefix, args.fps_log)
    Path(args.prefix+'.compiled-fight.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='stages'}, indent=2))
    for stage in result['stages']:
        print(json.dumps({k:v for k,v in stage.items() if k!='top_shaders'}, indent=2))
