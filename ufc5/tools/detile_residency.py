"""Summarize detile GPU scopes per surface from a production GPU timing CSV.

Usage: python detile_residency.py <production.csv> [<production.csv> ...]
Compare in-game detile time against game-closed replay medians (session 65).
"""
import collections
import csv
import sys

REPLAY_MS = {
    '0x0000001164920000': 0.084624, '0x00000011673b0000': 0.154032,
    '0x0000001167460000': 0.175520, '0x00000011687e0000': 0.085440,
    '0x0000001168bd0000': 0.249776, '0x00000011691a0000': 0.082608,
    '0x000000116abb0000': 0.477552, '0x0000001172520000': 0.174912,
}


def summarize(path):
    kinds = collections.defaultdict(lambda: [0, 0.0])
    surfaces = collections.defaultdict(lambda: [0, 0.0])
    frames = set()
    with open(path, newline='') as handle:
        for row in csv.DictReader(handle):
            frames.add(row['frame'])
            kind = row['kind']
            if kind.startswith('detile'):
                kinds[kind][0] += 1
                kinds[kind][1] += float(row['gpu_ms'])
                if kind == 'detile':
                    surfaces[row['arg4']][0] += 1
                    surfaces[row['arg4']][1] += float(row['gpu_ms'])
    count = max(len(frames), 1)
    print(f'== {path}: {len(frames)} epochs')
    for kind, (n, ms) in sorted(kinds.items(), key=lambda item: -item[1][1]):
        print(f'  {kind:16s} {n / count:6.1f}/epoch {ms / count:8.2f} ms/epoch')
    print('  surface              n/epoch  ms/each  replay_ms  inflation')
    for address, (n, ms) in sorted(surfaces.items(), key=lambda item: -item[1][1])[:12]:
        replay = REPLAY_MS.get(address)
        ratio = f'{ms / n / replay:7.1f}x' if replay else '      -'
        replay_text = f'{replay:9.3f}' if replay else '        -'
        print(f'  {address}  {n / count:6.1f}  {ms / n:7.3f}  {replay_text}  {ratio}')


for argument in sys.argv[1:]:
    summarize(argument)
