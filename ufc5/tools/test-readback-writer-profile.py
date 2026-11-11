"""Reject unsafe observational bounds before considering a transfer-readback port."""
import unittest
import tempfile
import json
from pathlib import Path
from readback_writer_profile import classify, WriterRanges, analyze_writers, writer_gpu_context


def event(kind, time, tick=1, address=0x1000, size=0, resource=0, tx=0, **extra):
    r = dict(kind=kind, begin_ns=time, end_ns=time, tick=tick, address=address,
             bytes=size, resource=resource, transaction=tx, thread=1, _scheduler=0)
    r.update(extra)
    return r


def fixture():
    return [event('writer_capture_floor', 1, resource=1),
            event('gpu_writer_fill_exact', 2, tick=2, size=128, resource=11),
            event('readback_dependency_checkpoint', 10, tick=6, size=5, resource=11, tx=1),
            event('readback_required_bytes', 11, tick=6, size=128, resource=11, tx=1),
            event('timeline_wait', 12, tick=6, size=6, resource=0, tx=1, end_ns=20),
            event('buffer_readback_sync', 9, tick=6, size=128, resource=0x1000, tx=1, end_ns=21)]


class WriterTests(unittest.TestCase):
    def decision(self, rows, submits=()):
        return classify(rows, submits)[0]

    def test_known_submitted_earlier_writer(self):
        r = self.decision(fixture())
        self.assertTrue(r['dependency_bound_eligible'])
        self.assertTrue(r['earlier_than_queue_tail'])
        self.assertEqual(r['conservative_needed_tick'], 2)
        self.assertEqual(r['intervening_submitted_batches'], 3)

    def test_partial_and_disjoint_writer_coverage(self):
        rows = fixture(); rows[1]['bytes'] = 64
        self.assertIn('missing_writer_coverage', self.decision(rows)['reasons'])
        rows += [event('gpu_writer_copy_exact', 3, tick=4, address=0x1040, size=64, resource=11)]
        self.assertEqual(self.decision(rows)['conservative_needed_tick'], 4)

    def test_writers_in_unsubmitted_buffer_block(self):
        rows = fixture(); rows[1]['tick'] = 6
        self.assertIn('required_tick_unsubmitted', self.decision(rows)['reasons'])

    def test_opaque_bda_bounds_all_ranges(self):
        rows = fixture() + [event('gpu_writer_address_unknown', 3, tick=4),
                            event('writer_operation_recorded', 4, tick=4)]
        self.assertEqual(self.decision(rows)['conservative_needed_tick'], 4)
        rows[-1]['tick'] = 6
        self.assertIn('required_tick_unsubmitted', self.decision(rows)['reasons'])
        rows.pop()
        self.assertIn('opaque_operation_pending', self.decision(rows)['reasons'])

    def test_bind_pending_and_declared_operation_completion(self):
        rows = fixture() + [event('gpu_writer_bind_pending', 3, tick=3, size=128, resource=11),
                            event('gpu_writer_compute_declared', 4, tick=3, size=128, resource=11)]
        self.assertIn('overlapping_bound_write_pending', self.decision(rows)['reasons'])
        rows += [event('writer_operation_recorded', 5, tick=4)]
        r = self.decision(rows)
        self.assertTrue(r['dependency_bound_eligible'])
        self.assertEqual(r['observed_range_writer_tick'], 4)

    def test_buffer_retirement_and_mapping_removal(self):
        for kind in ('buffer_owner_retire', 'mapped_range_removed'):
            rows = fixture() + [event(kind, 3, size=128, resource=11)]
            self.assertIn('missing_writer_coverage', self.decision(rows)['reasons'])

    def test_handle_identity_and_packed_footprint(self):
        rows = fixture(); rows[3]['resource'] = 12
        self.assertIn('source_identity_mismatch', self.decision(rows)['reasons'])
        rows = fixture(); rows[3]['bytes'] = 65
        self.assertTrue(self.decision(rows)['dependency_bound_eligible'])  # 64-byte packing remains128
        rows[3]['bytes'] = 64
        self.assertIn('incomplete_copy_footprint', self.decision(rows)['reasons'])

    def test_capture_reset_and_floor(self):
        rows = fixture() + [event('writer_capture_floor', 3, resource=4)]
        self.assertIn('missing_writer_coverage', self.decision(rows)['reasons'])
        rows += [event('gpu_writer_upload_exact', 4, tick=4, size=128, resource=11)]
        self.assertEqual(self.decision(rows)['conservative_needed_tick'], 4)
        rows = fixture(); rows.pop(0)
        self.assertIn('missing_capture_floor', self.decision(rows)['reasons'])

    def test_submit_return_must_precede_checkpoint(self):
        submits = [dict(_scheduler=0, tick=2, submit_end_ns=15)]
        self.assertIn('submit_not_returned_at_checkpoint', self.decision(fixture(), submits)['reasons'])

    def test_interval_replacement_preserves_neighbors(self):
        ranges = WriterRanges(); ranges.assign(0, 128, {'tick': 1}); ranges.assign(32, 64, {'tick': 2})
        self.assertEqual([r['tick'] for r in ranges.covering(0, 128)], [1, 2, 1])
        ranges.remove(48, 80)
        self.assertIsNone(ranges.covering(0, 128))

    def test_multiple_recording_threads_block_order_assumption(self):
        rows = fixture() + [event('gpu_writer_copy_exact', 3, size=64, resource=11, thread=2)]
        self.assertIn('multiple_recording_threads', self.decision(rows)['reasons'])

    def test_event_drops_withhold_eligibility(self):
        class Store:
            def query(self, *args): return fixture()
        with tempfile.TemporaryDirectory() as tmp:
            report = analyze_writers(Store(), [], Path(tmp), [{'event_drops': 1}])
            self.assertEqual(report['addresses'][0x1000]['eligible'], 0)
            self.assertEqual(report['reasons']['capture_event_drops'], 1)

    def test_gpu_context_wrap_identity_and_rejected_bound(self):
        decision = self.decision(fixture())
        def gpu(kind, tick, start, end, scheduler=0, tx=0):
            return dict(kind=kind, tick=tick, start_raw=start, end_raw=end,
                        timestamp_mask=255, timestamp_period_ns=1000000,
                        gpu_ms=(end-start) & 255, _scheduler=scheduler, transaction=tx)
        ranges = [gpu('submit', 2, 240, 250), gpu('submit', 5, 2, 5),
                  gpu('submit', 6, 7, 12), gpu('copy_target_readback', 6, 8, 10, tx=1),
                  gpu('submit', 2, 30, 70, scheduler=1)]
        r = writer_gpu_context([decision], ranges)['addresses'][0x1000]
        self.assertEqual(r['writer_batch_end_to_copy_start']['median_ms'], 14)
        self.assertEqual(r['writer_batch_end_to_tail_end']['median_ms'], 11)
        self.assertEqual(r['needed_writer_batch_elapsed']['median_ms'], 10)
        decision['dependency_bound_eligible'] = False
        r = writer_gpu_context([decision], ranges)['addresses'][0x1000]
        self.assertEqual(r['writer_batch_end_to_copy_start']['pairs'], 0)
        self.assertEqual(r['copy_elapsed']['median_ms'], 2)


if __name__ == '__main__':
    unittest.main()
