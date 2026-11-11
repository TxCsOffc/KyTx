"""Summarize aggregate descriptor A/B windows; shadow time includes validation work."""
import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('csv', type=Path)
parser.add_argument('--after-host-ns', type=int, default=0,
                    help='Include only windows that start after this steady-clock point.')
args = parser.parse_args()
groups = defaultdict(list)
with args.csv.open(newline='', encoding='utf-8-sig') as f:
    for row in csv.DictReader(f):
        if row['host_ns'] == 'host_ns':  # Allow headers from separate thread log handles.
            continue
        if (float(row['window_ms']) >= 1500 and int(row['materializations']) > 0 and
                int(row['host_ns']) - float(row['window_ms'])*1e6 >= args.after_host_ns):
            groups[(row['thread'], row['mode'])].append(row)
print('Materializer CPU elapsed includes preemption; shadow additionally includes checks.')
print('Select only the same stable fight scene. These numbers do not establish an FPS gain.')
print('thread,mode,windows,materializations,median_cpu_us_per_call,weighted_cpu_us_per_call,'
      'eligible_percent,fast_materializations,blocked_materializations,descriptor_checks,'
      'descriptor_mismatches,full_checks,full_mismatches')
for (thread, mode), rows in sorted(groups.items()):
    total = lambda k: sum(int(r[k]) for r in rows)
    count = total('materializations')
    per_call = [float(r['cpu_ms'])*1000/int(r['materializations']) for r in rows]
    fields = [thread, mode, len(rows), count, f'{statistics.median(per_call):.3f}',
              f'{sum(float(r["cpu_ms"]) for r in rows)*1000/count:.3f}',
              f'{100*total("eligible_calls")/max(1,total("descriptor_calls")):.2f}']
    fields += [total(k) for k in ('fast_materializations','blocked_materializations',
                                 'descriptor_checks','descriptor_mismatches','full_checks','full_mismatches')]
    print(','.join(map(str, fields)))
