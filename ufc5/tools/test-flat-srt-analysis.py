import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('flat_srt_analysis', Path(__file__).with_name('analyze-flat-srt.py'))
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


class WindowSelection(unittest.TestCase):
    def test_all_shader_summary_does_not_duplicate_checks_or_hide_shadow_cost(self):
        base = dict(host_ns=3_000_000_000,thread=1,mode='on',window_ms=2000,
                    descriptor_calls=0,eligible_calls=0,fallback_calls=0,eligible_words=0,
                    descriptor_checks=0,descriptor_mismatches=0,blocked_materializations=0,
                    raw_calls=30,raw_eligible=30,raw_fallback=0,recipe_evaluations=30,
                    user_data_evaluations=60,memo_hits=20,reference_shadow_ms=0.1,candidate_shadow_ms=0.1,
                    scope='all',no_recipe_materializations=0)
        rows=[]
        for shader,kind,factor in [('0x1','shader',1),('0x2','shader',2),('all','summary',3)]:
            row=dict(base,shader=shader,record_kind=kind,materializations=10*factor,cpu_ms=0.3*factor,
                     fast_materializations=8*factor,shadow_materializations=factor,reference_materializations=factor,
                     full_checks=factor,full_mismatches=factor,fast_cpu_ms=0.1*factor,
                     reference_cpu_ms=0.05*factor,shadow_cpu_ms=0.15*factor,fast_recipe_evaluations=8*factor)
            rows.append(row)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'all.csv'
            with path.open('w',newline='') as output:
                writer=csv.DictWriter(output,fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
            result=analyzer.analyze(path)
        self.assertEqual(result['full_checks_all_rows'],3)
        self.assertEqual(result['full_mismatches_all_rows'],3)
        total=next(g for g in result['groups'] if g['record_kind']=='summary')
        self.assertEqual(total['materializations'],30)
        self.assertTrue(total['count_closure_ok'] and total['elapsed_closure_ok'])
        self.assertAlmostEqual(total['shadow_cpu_ms'],0.45)
        self.assertAlmostEqual(total['fast_us_per_call'],12.5)

    def test_edges_mismatches_and_actual_execution(self):
        fields = ['host_ns', 'thread', 'shader', 'mode', 'window_ms', 'materializations',
                  'cpu_ms', 'fast_materializations', 'blocked_materializations', 'recipe_evaluations',
                  'raw_calls', 'raw_eligible', 'full_checks', 'full_mismatches', 'candidate_shadow_ms']
        def row(end, duration, mode, cpu, fast, blocked, mismatch=0):
            return dict(zip(fields, [end, 1, '0x1', mode, duration, 100, cpu, fast, blocked,
                                    fast*10, 1000, 1000, 1, mismatch, 0]))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'experiment.csv'
            with path.open('w', newline='') as target:
                writer = csv.DictWriter(target, fieldnames=fields)
                writer.writeheader()
                writer.writerows([
                    row(1_000_000_000, 2000, 'off', 90, 0, 0),  # before selected fight
                    row(4_000_000_000, 2000, 'off', 1, 0, 0),
                    row(4_500_000_000, 500, 'shadow', 3, 0, 0, 1),  # mismatch in short transition
                    row(6_500_000_000, 2000, 'on', 0.8, 90, 10),
                ])
            result = analyzer.analyze(path, start_ns=2_000_000_000, end_ns=7_000_000_000)
        self.assertEqual(result['full_mismatches_all_rows'], 1)
        self.assertEqual(result['full_checks_all_rows'], 3)
        self.assertEqual(len(result['groups']), 2)
        off, on = result['groups']
        self.assertEqual(off['weighted_us_per_call'], 10)
        self.assertEqual(on['weighted_us_per_call'], 8)
        self.assertEqual(on['fast_materializations'], 90)
        self.assertEqual(on['blocked_materializations'], 10)


if __name__ == '__main__':
    unittest.main()
