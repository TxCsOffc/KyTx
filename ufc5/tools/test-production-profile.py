"""Check analysis attribution against deliberately overlapping, wrapped two-scheduler data."""
import contextlib
import csv
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('profile_analysis', Path(__file__).with_name('analyze-production-profile.py'))
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
from focused_profile import Ranges, fine_calls, lifetime_rows
from materializer_profile import analyze_materializer


class ProfileAnalysisTest(unittest.TestCase):
    def test_materializer_sidecar_keeps_threads_and_overlapping_categories_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            source=root/'materializer.csv'
            rows=[]
            for thread in (42,43):
                def row(kind,**values):
                    result=dict(kind=kind,host_ns=100,thread=thread,shader='0xabc',plan='0xdef',calls=0,
                                samples=2,cpu_ns=0,phase='',opcode='',words=0,hits=0,misses=0,failures=0,
                                source=0,dword=0,metadata='')
                    result.update(values); rows.append(result)
                row('aggregate',calls=64,cpu_ns=1_000_000,metadata='sample_cpu_ns=100000;sample_every=32')
                row('phase',cpu_ns=80_000,phase='buffers')
                row('opcode',calls=12,cpu_ns=40_000,opcode='ReadConst',words=4,hits=8,misses=4)
            row('marker',metadata='complete')
            profile.write_csv(source,rows)
            analyze_materializer(source,root/'analysis')
            with (root/'analysis'/'materializer-shaders.csv').open() as f:
                shaders=list(csv.DictReader(f))
            self.assertEqual(len(shaders),2)
            self.assertTrue(all(float(r['recorded_setup_remainder_ms'])==.02 for r in shaders))
            self.assertTrue(all(r['sample_closure_ok']=='True' for r in shaders))
            with (root/'analysis'/'materializer-opcodes.csv').open() as f:
                ops=list(csv.DictReader(f))
            self.assertTrue(all(float(r['mean_word_elapsed_us'])==10 for r in ops))

    def test_probe_emission_is_excluded_from_parent_self(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root=Path(tmp)
            base=self.fixture(root,False)
            path=Path(str(base)+'.events.csv')
            with path.open() as f:
                rows=[r for r in csv.DictReader(f) if not r['kind'].startswith('profile_') and r['kind'] not in profile.WAIT_KINDS]
            rows += [self.event('draw_state_cpu',100,110,42),
                     self.event('cpu_prepare_draw_state',100,110,42),
                     self.event('cpu_materialize_resources',101,103,42)]
            for r in rows: r['probe_end_ns']=0
            rows[-1]['probe_end_ns']=105_000_000
            profile.write_csv(path,rows)
            profile.analyze(base,root/'analysis')
            with (root/'analysis'/'cpu-fine-calls.csv').open() as f:
                calls=list(csv.DictReader(f))
            parent=next(r for r in calls if r['kind']=='cpu_prepare_draw_state')
            child=next(r for r in calls if r['kind']=='cpu_materialize_resources')
            self.assertEqual(float(parent['inclusive_net_ms']),8)
            self.assertEqual(float(parent['self_net_ms']),6)
            self.assertEqual(float(child['self_net_ms']),2)

    def test_scaled_cpu_budget_is_withheld_when_it_exceeds_phase(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root=Path(tmp)
            base=self.fixture(root,False)
            path=Path(str(base)+'.events.csv')
            with path.open() as f:
                rows=list(csv.DictReader(f))
            rows += [self.event('draw_state_cpu',100,106,42),self.event('cpu_materialize_resources',101,103,42),
                     self.event('cpu_buffer_obtain',107,108,42)]
            profile.write_csv(path,rows)
            Path(str(base)+'.manifest.json').write_text(json.dumps({'fine_cpu_root_every':32}))
            profile.analyze(base,root/'analysis')
            with (root/'analysis'/'cpu-sampling-closure.csv').open() as f:
                closure=list(csv.DictReader(f))[0]
            self.assertEqual(closure['withhold_scaled_budget'],'True')
            self.assertGreater(float(closure['ratio']),1.25)
            with (root/'analysis'/'cpu-fine-summary.csv').open() as f:
                result=next(r for r in csv.DictReader(f) if r['kind']=='cpu_materialize_resources')
            self.assertEqual(result['scaled_budget_usable'],'False')
            with (root/'analysis'/'cpu-fine-summary.csv').open() as f:
                outside=next(r for r in csv.DictReader(f) if r['kind']=='cpu_buffer_obtain')
            self.assertEqual(outside['phase'],'outside_draw')
            self.assertEqual(outside['scaled_budget_usable'],'False')

    def test_fine_attribution_conserves_nested_time_and_wait_union(self):
        rows = [self.event('draw_state_cpu', 0, 10, 42),
                self.event('cpu_prepare_draw_state', 0, 10, 42),
                self.event('cpu_program_get', 1, 6, 42),
                self.event('cpu_materialize_resources', 2, 4, 42)]
        calls = fine_calls(rows, Ranges([(3_000_000, 5_000_000), (4_000_000, 6_000_000)]))
        self.assertAlmostEqual(sum(r['self_net_ms'] for r in calls), 7)
        by_kind = {r['kind']: r for r in calls}
        self.assertEqual(by_kind['cpu_program_get']['inclusive_net_ms'], 2)
        self.assertEqual(by_kind['cpu_program_get']['self_net_ms'], 1)
        self.assertTrue(all(r['phase']=='state' for r in calls))

    def test_lifetime_preserves_unknown_writer_and_exact_publication(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'events.csv'
            rows=[]
            def add(kind, t, address, size, resource=0, tx=0, end=None):
                r=self.event(kind,t,t if end is None else end,42)
                r.update(address=hex(address),bytes=str(size),resource=hex(resource),transaction=tx,context='metadata')
                rows.append(r)
            addr=0x1164b80000
            add('gpu_writer_compute_declared',1,addr,524288,11)
            add('gpu_writer_address_unknown',2,addr,524288)
            add('readback_read_request',3,addr,1,addr,7)
            add('readback_widened_window',3,addr,524288,11,7)
            add('timeline_wait',4,addr,20,0,7,end=5)
            add('readback_content_equal',5.1,addr,524288,123,7)
            add('readback_published',5.2,addr,524288,0,7)
            add('buffer_readback_sync',3.1,addr,524288,addr,7,end=5.3)
            add('cpu_consume_metadata_code',5.4,addr,1)
            profile.write_csv(path,rows)
            store=profile.EventStore(Path(tmp)/'events.sqlite',[path])
            store.add(path,0,'producer')
            store.index()
            result=lifetime_rows(store,[],[],0)[0]
            store.db.close()
            self.assertEqual(result['packed_bytes'],524288)
            self.assertEqual(result['requested_bytes'],1)
            self.assertEqual(result['exact_content_results'],'equal')
            self.assertEqual(result['wait_union_ms'],1)
            self.assertIn('gpu_writer_compute_declared',result['last_known_gpu_writer'])
            self.assertIn('gpu_writer_address_unknown',result['last_opaque_gpu_writer'])
            self.assertIn('cpu_consume_metadata_code',result['first_instrumented_cpu_consumer'])

    def test_overlapping_intervals_are_not_added(self):
        self.assertEqual(profile.duration([(0, 6_000_000), (3_000_000, 9_000_000)]), 9)
        self.assertEqual(profile.duration(profile.overlap([(0, 6_000_000)], [(3_000_000, 9_000_000)])), 3)

    def fixture(self, root, detailed):
        base = root / 'trace.csv'
        for sid, role in [(0, 'producer'), (1, 'present')]:
            path = base if sid == 0 else Path(str(base) + '.scheduler-1.csv')
            profile.write_csv(Path(str(path) + '.meta.csv'), [{'role': role, 'queue_handle': 'same_queue'}])
            gpu = []
            for kind, a, b in ([('submit', 250, 4), ('render', 252, 2), ('detile_dispatch', 254, 1)] if sid == 0 else [('submit', 5, 6)]):
                gpu.append(dict(kind=kind, frame=10, shader='0', tick=1, gpu_ms=(b-a)&255,
                                start_raw=a, end_raw=b, timestamp_mask=255, timestamp_period_ns=1_000_000,
                                split=0, transaction=0))
            profile.write_csv(path, gpu)
            cpu = dict(frame=10, wall_ms=10, begin_ns=100_000_000, end_ns=110_000_000,
                       draws=1, timer_drops=0, event_drops=0)
            cpu.update({k: 0 for k in ('draw_state_ms','draw_bindings_ms','draw_vertex_index_ms','draw_pipeline_ms','draw_targets_ms','draw_commit_ms')})
            profile.write_csv(Path(str(path) + '.cpu.csv'), [cpu])
            submit = dict(frame=10, tick=1, queue_handle='same_queue', thread=42 if sid == 0 else 44,
                          submit_begin_ns=107_000_000, queue_enter_ns=107_050_000, submit_end_ns=107_100_000,
                          build_begin_ns=100_000_000, draws=1, guest_computes=0, wait_dependencies=0,
                          known_idle_since_ns=0, timer_page=0)
            profile.write_csv(Path(str(path) + '.submits.csv'), [submit])
            events = []
            if sid == 0:
                for kind, a, b, thread in [('guest_process_slice',100,110,42), ('timeline_wait',103,105,42),
                                          ('timeline_wait',101,109,43), ('profile_collect_cpu',106,106.2,42)]:
                    events.append(self.event(kind,a,b,thread))
                if detailed:
                    events.append(self.event('draw_state',100,106,42))
            # Include a real event header even for the empty presenter file.
            if events:
                profile.write_csv(Path(str(path) + '.events.csv'), events)
            else:
                Path(str(path) + '.events.csv').write_text(','.join(profile.EventStore.columns[:-2]) + '\n')
        # Sidecars must never be mistaken for an additional GPU scheduler.
        Path(str(base) + '.scheduler-1.csv.stdout.csv').write_text('malformed sidecar\n')
        return base

    def event(self, kind, a, b, thread):
        row = {k: '0' for k in profile.EventStore.columns if not k.startswith('_')}
        row.update(kind=kind, frame=10, begin_ns=int(a*1_000_000), end_ns=int(b*1_000_000),
                   thread=thread, file='fixture.cpp', line=1)
        return row

    def test_thread_separation_timestamp_wrap_and_nested_scopes(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp)
            row = profile.analyze(self.fixture(root, True), root/'analysis')[0]
            self.assertAlmostEqual(row['foreground_explicit_wait_ms'], 2)
            self.assertAlmostEqual(row['guest_cpu_excluding_wait_submit_profile_ms'], 7.7)
            self.assertAlmostEqual(row['recording_excluding_waits_ms'], 4)
            self.assertAlmostEqual(row['translation_other_cpu_ms'], 3.7)
            self.assertAlmostEqual(row['gpu_batch_elapsed_union_ms'], 11)
            self.assertAlmostEqual(row['graphics_scope_union_ms'], 6)
            self.assertAlmostEqual(row['detile_dispatch_scope_union_ms'], 3)
            self.assertEqual(row['command_buffers'], 2)
            self.assertEqual(row['uncollected_batches'], 0)

    def test_unsampled_draw_detail_is_unavailable(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp)
            row = profile.analyze(self.fixture(root, False), root/'analysis')[0]
            self.assertIsNone(row['translation_other_cpu_ms'])
            self.assertIsNone(row['recording_excluding_waits_ms'])

    def test_readback_joins_resource_metadata_and_only_its_thread_waits(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp)
            base = self.fixture(root, False)
            path = Path(str(base) + '.events.csv')
            with path.open() as f:
                rows = list(csv.DictReader(f))
            rows += [self.event('buffer_readback_sync',100,106,42),
                     self.event('readback_source_allocation',101,101,42),
                     self.event('allocation_memory_properties',101,101,42),
                     self.event('timeline_wait',102,104,43)]
            for r in rows:
                if r['kind'] in {'buffer_readback_sync','readback_source_allocation',
                                 'allocation_memory_properties','timeline_wait'}:
                    r['transaction'] = 7
                if r['kind'] == 'buffer_readback_sync':
                    r.update(resource='0x1234', bytes=128)
                elif r['kind'] == 'readback_source_allocation':
                    r.update(resource='0xabcd', bytes=4096)
                elif r['kind'] == 'allocation_memory_properties':
                    r.update(resource='0x1', bytes=2)
                elif r['kind'] == 'timeline_wait':
                    r.update(bytes=8)
            profile.write_csv(path, rows)
            profile.analyze(base, root/'analysis')
            with (root/'analysis'/'readbacks.csv').open() as f:
                readback = next(csv.DictReader(f))
            self.assertEqual(readback['guest_address'], '0x1234')
            self.assertEqual(readback['packed_bytes'], '128')
            self.assertEqual(readback['source_memory_properties'], '0x1')
            self.assertEqual(readback['source_memory_type_index'], '2')
            self.assertEqual(readback['wait_batch_ticks'], '8')
            self.assertEqual(float(readback['host_wait_union_ms']), 2)


if __name__ == '__main__':
    unittest.main()
