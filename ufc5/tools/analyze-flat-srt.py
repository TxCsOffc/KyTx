"""Aggregate live FlatSRT windows without assigning a shader-local saving to FPS."""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def analyze(path, minimum_ms=1500, start_ns=None, end_ns=None):
    groups = defaultdict(list)
    mismatches = checks = skipped = 0
    with Path(path).open(newline="", encoding="utf-8-sig") as source:
        for row in csv.DictReader(source):
            try:
                if None in row or not row.get("candidate_shadow_ms"):
                    skipped += 1
                    continue  # incomplete trailing row
                end = int(row['host_ns'])
                begin = end - float(row['window_ms']) * 1e6
                if (start_ns is not None and end < start_ns) or (end_ns is not None and begin > end_ns):
                    continue
                if row.get('record_kind') != 'summary':
                    mismatches += int(row["full_mismatches"])
                    checks += int(row["full_checks"])
                calls = int(row["materializations"])
                contained = (start_ns is None or begin >= start_ns) and (end_ns is None or end <= end_ns)
                if contained and float(row["window_ms"]) >= minimum_ms and calls:
                    groups[(row["thread"], row["shader"], row["mode"], row.get('scope','legacy'))].append(row)
            except (ValueError, TypeError):
                skipped += 1
                continue
    result = {"full_checks_all_rows": checks, "full_mismatches_all_rows": mismatches,
              "skipped_incomplete_or_invalid_rows": skipped, "start_ns": start_ns, "end_ns": end_ns, "groups": []}
    for (thread, shader, mode, scope), rows in sorted(groups.items()):
        calls = sum(int(r["materializations"]) for r in rows)
        cpu = sum(float(r["cpu_ms"]) for r in rows)
        group = {
            "thread": thread, "shader": shader, "mode": mode, "scope": scope, "stable_windows": len(rows),
            "materializations": calls, "cpu_ms": cpu, "weighted_us_per_call": cpu * 1000 / calls,
            "median_window_us_per_call": statistics.median(float(r["cpu_ms"]) * 1000 / int(r["materializations"]) for r in rows),
            "fast_materializations": sum(int(r["fast_materializations"]) for r in rows),
            "blocked_materializations": sum(int(r["blocked_materializations"]) for r in rows),
            "recipe_evaluations": sum(int(r["recipe_evaluations"]) for r in rows),
            "raw_calls": sum(int(r["raw_calls"]) for r in rows),
            "raw_eligible": sum(int(r["raw_eligible"]) for r in rows),
        }
        if 'record_kind' in rows[0]:
            for field in ['shadow_materializations','reference_materializations','no_recipe_materializations','fast_recipe_evaluations']:
                group[field] = sum(int(r[field]) for r in rows)
            for field in ['fast_cpu_ms','reference_cpu_ms','shadow_cpu_ms']:
                group[field] = sum(float(r[field]) for r in rows)
            fast = group['fast_materializations']
            reference = group['reference_materializations']
            group['fast_us_per_call'] = group['fast_cpu_ms']*1000/fast if fast else None
            group['reference_us_per_call'] = group['reference_cpu_ms']*1000/reference if reference else None
            group['count_closure_ok'] = fast+reference+group['shadow_materializations']==calls
            group['elapsed_closure_ok'] = abs(cpu-group['fast_cpu_ms']-group['reference_cpu_ms']-group['shadow_cpu_ms'])<0.01
            group['record_kind'] = rows[0]['record_kind']
        result["groups"].append(group)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv")
    parser.add_argument("--minimum-ms", type=float, default=1500)
    parser.add_argument("--start-ns", type=int)
    parser.add_argument("--end-ns", type=int)
    args = parser.parse_args()
    print(json.dumps(analyze(args.csv, args.minimum_ms, args.start_ns, args.end_ns), indent=2))
