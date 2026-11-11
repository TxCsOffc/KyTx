"""Freeze selected fight rows and summarize mode windows and presentation log slices."""
import argparse
import csv
import importlib.util
import json
import re
import statistics
from pathlib import Path

spec = importlib.util.spec_from_file_location('flat_analysis', Path(__file__).with_name('analyze-flat-srt.py'))
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('prefix')
parser.add_argument('--fps-log', default=r'D:\PS5\fps-pressure-test.txt')
args = parser.parse_args()
prefix = args.prefix
markers = [json.loads(line) for line in Path(prefix+'.flat-stages.jsonl').read_text(encoding='utf-8-sig').splitlines()]
start, end = int(markers[0]['host_ns']), int(markers[-1]['host_ns'])
path = Path(prefix+'.flat-srt.csv')
frozen = Path(prefix+'.flat-first-attempt.csv')
with path.open(newline='') as source, frozen.open('w', newline='') as target:
    reader = csv.DictReader(source)
    writer = csv.DictWriter(target, fieldnames=reader.fieldnames)
    writer.writeheader()
    for row in reader:
        if None in row:
            continue
        row_end = int(row['host_ns'])
        row_begin = row_end-float(row['window_ms'])*1e6
        if row_end >= start and row_begin <= end:
            writer.writerow(row)
result = analyzer.analyze(frozen, start_ns=start, end_ns=end)
result['stages'] = []
fps = Path(args.fps_log).read_bytes()
pattern = re.compile(r'PresentRate: pressure=(\d+) frames=(\d+) seconds=([\d.]+) fps=([\d.]+)')
for a,b in zip(markers, markers[1:]):
    stage = analyzer.analyze(frozen, start_ns=int(a['host_ns']), end_ns=int(b['host_ns']))
    stage['stage'] = a['stage']
    stage['duration_seconds'] = (b['host_ns']-a['host_ns'])/1e9
    text = fps[int(a['fps_bytes']):int(b['fps_bytes'])].decode('utf-8', errors='replace')
    samples = pattern.findall(text)[1:]  # first report may span the mode boundary
    samples = [s for s in samples if s[0]=='0']
    stage['fps_samples_excluding_first_boundary_report'] = len(samples)
    if samples:
        values = [float(s[3]) for s in samples]
        stage['present_rate_median'] = statistics.median(values)
        stage['present_rate_min'] = min(values)
        stage['present_rate_max'] = max(values)
        stage['present_rate_weighted'] = sum(int(s[1]) for s in samples)/sum(float(s[2]) for s in samples)
    result['stages'].append(stage)
result['limits'] = 'Shader-local elapsed A/B, with observer/preemption costs. Presentation rates drift across stages; no whole-game FPS gain inferred. Epoch profiling stayed disarmed.'
Path(prefix+'.flat-first-attempt.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
