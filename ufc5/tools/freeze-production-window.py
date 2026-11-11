"""Freeze one producer epoch range from a rearmed production trace."""
import argparse
import csv
import json
import re
from pathlib import Path


def freeze(source, target, minimum=0, maximum=None):
    with Path(str(source)+'.cpu.csv').open(newline='', encoding='utf-8-sig') as stream:
        epochs = [int(r['frame']) for r in csv.DictReader(stream)
                  if None not in r and r.get('frame') and int(r['frame']) >= minimum and
                  (maximum is None or int(r['frame']) <= maximum)]
    if not epochs:
        raise ValueError('No complete producer epoch rows in the requested range')
    lo, hi = min(epochs), max(epochs)
    paths = [source] + sorted(p for p in source.parent.glob(source.name+'.scheduler-*.csv')
        if re.fullmatch(re.escape(source.name)+r'\.scheduler-\d+\.csv', p.name))
    result = {'source': str(source), 'target': str(target), 'first_epoch': lo,
              'last_epoch': hi, 'producer_epochs': len(epochs), 'files': []}
    for path in paths:
        scheduler_suffix = path.name[len(source.name):]
        for suffix in ('', '.cpu.csv', '.events.csv', '.submits.csv', '.meta.csv'):
            src = Path(str(path)+suffix)
            dst = Path(str(target)+scheduler_suffix+suffix)
            if not src.exists():
                continue
            count = 0
            with src.open(newline='', encoding='utf-8-sig') as incoming, dst.open('w', newline='', encoding='utf-8') as outgoing:
                reader = csv.DictReader(incoming)
                if not reader.fieldnames:
                    continue
                writer = csv.DictWriter(outgoing, fieldnames=reader.fieldnames)
                writer.writeheader()
                for row in reader:
                    if None in row or any(v is None for v in row.values()):
                        continue
                    if 'frame' in row and not lo <= int(row['frame']) <= hi:
                        continue
                    writer.writerow(row)
                    count += 1
            result['files'].append({'path': str(dst), 'rows': count})
    Path(str(target)+'.selection.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('target', type=Path)
    parser.add_argument('--minimum-epoch', type=int, default=0)
    parser.add_argument('--maximum-epoch', type=int)
    args = parser.parse_args()
    if args.source.resolve()==args.target.resolve():
        parser.error('Source and target must differ')
    print(json.dumps(freeze(args.source, args.target, args.minimum_epoch, args.maximum_epoch), indent=2))
