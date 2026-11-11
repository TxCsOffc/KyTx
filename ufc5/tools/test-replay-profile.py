import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('replay_profile', Path(__file__).with_name('analyze-replay-profile.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def write_csv(path, rows):
    with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


class ReplayJoins(unittest.TestCase):
    def fixture(self, root):
        replay, profile = root / 'replay.csv', root / 'gpu.csv'
        Path(str(replay) + '.meta.json').write_text(json.dumps({'replay_pid': 42}))
        write_csv(Path(str(replay) + '.iterations.csv'), [dict(
            capture='capture.kdr', iteration='0', warmup='0', frame='0', tick='2', thread='99',
            flush_begin_ns='100001000', flush_end_ns='100004000', gpu_ms='0.5', wall_ms='0.6')])
        for n, enter in enumerate((100002000, 100012000)):
            source = Path(str(profile) + f'.scheduler-{n}.csv')
            write_csv(Path(str(source) + '.meta.csv'), [dict(role='replay')])
            write_csv(Path(str(source) + '.submits.csv'), [dict(
                tick='2', thread='99', queue_enter_ns=str(enter), submit_end_ns=str(enter+500))])
            write_csv(Path(str(source) + '.cpu.csv'), [dict(timer_drops='0', event_drops='0')])
            write_csv(source, [dict(tick='2', kind='detile', split='0')])
        packets = root / 'packets.json'
        queue = dict(type=0, thread=99, process='replay ( 42 )', a=2, b=3, context='ctx', sequence='1')
        phase = dict(context='ctx', sequence='1', dma_residence_span_us=1)
        packets.write_text(json.dumps({'queues': [queue], 'queue_phases': [phase]}))
        return replay, profile, packets, queue

    def test_reused_tick_and_native_pid_qpc_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, profile, packets, queue = self.fixture(Path(directory))
            result = module.analyze(replay, profile, packets, 1_000_000)
            self.assertEqual(result['submit_matches'], 1)
            self.assertEqual(result['joins'][0]['native_match_count'], 1)
            self.assertEqual(result['joins'][0]['native_phase']['dma_residence_span_us'], 1)
            queue['process'] = 'different ( 142 )'
            packets.write_text(json.dumps({'queues': [queue], 'queue_phases': []}))
            result = module.analyze(replay, profile, packets, 1_000_000)
            self.assertEqual(result['joins'][0]['native_match_count'], 0)

    def test_ambiguous_native_join_not_used_and_raw_origin_required(self):
        with tempfile.TemporaryDirectory() as directory:
            replay, profile, packets, queue = self.fixture(Path(directory))
            packets.write_text(json.dumps({'queues': [queue, dict(queue, sequence='2')], 'queue_phases': []}))
            result = module.analyze(replay, profile, packets, 1_000_000)
            self.assertEqual(result['joins'][0]['native_match_count'], 2)
            self.assertNotIn('native_queue', result['joins'][0])
            with self.assertRaises(ValueError):
                module.analyze(replay, profile, packets)


if __name__ == '__main__':
    unittest.main()
