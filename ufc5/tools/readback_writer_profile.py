"""Conservative observed dependency bounds, using the existing lifetime recorder.

This is an eligibility study, not permission to change synchronization. Queue
sharing, publication and actual writer coverage still require implementation proof.
"""
from collections import Counter, defaultdict
import csv
import json
import statistics


def n(r, key):
    value = r.get(key, 0)
    return value if isinstance(value, int) else int(value, 0) if value else 0


def intersects(a, size, b, other):
    return a < b + other and b < a + size


class WriterRanges:
    def __init__(self):
        self.ranges = []

    def remove(self, begin, end):
        result = []
        for a, b, writer in self.ranges:
            if b <= begin or a >= end:
                result.append((a, b, writer))
            else:
                if a < begin:
                    result.append((a, begin, writer))
                if b > end:
                    result.append((end, b, writer))
        self.ranges = sorted(result)

    def assign(self, begin, end, writer):
        self.remove(begin, end)
        self.ranges.append((begin, end, writer))
        self.ranges.sort(key=lambda r: r[0])

    def covering(self, begin, end):
        cursor, writers = begin, []
        for a, b, writer in self.ranges:
            if b <= cursor:
                continue
            if a > cursor:
                return None
            writers.append(writer)
            cursor = max(cursor, b)
            if cursor >= end:
                return writers
        return None


def classify(events, submits):
    events = sorted(events, key=lambda r: (n(r, 'begin_ns'), n(r, 'end_ns')))
    recording_threads = defaultdict(set)
    for r in events:
        if r['kind'].startswith('gpu_writer_') or r['kind'] == 'writer_operation_recorded':
            recording_threads[n(r, '_scheduler')].add(n(r, 'thread'))
    transactions = defaultdict(list)
    for r in events:
        if n(r, 'transaction'):
            transactions[(n(r, '_scheduler'), n(r, 'transaction'))].append(r)
    submission = {(n(r, '_scheduler'), n(r, 'tick')): r for r in submits}
    states = defaultdict(lambda: dict(floor=None, opaque=0, opaque_pending=False,
        pending=[], declared=[], buffers=defaultdict(WriterRanges)))
    result = []
    for r in events:
        sid, kind = n(r, '_scheduler'), r['kind']
        state = states[sid]
        address, size, handle = n(r, 'address'), n(r, 'bytes'), n(r, 'resource')
        if kind == 'writer_capture_floor':
            state = states[sid] = dict(floor=handle, opaque=handle, opaque_pending=False,
                pending=[], declared=[], buffers=defaultdict(WriterRanges))
        elif kind == 'gpu_writer_address_unknown':
            state['opaque_pending'] = True
        elif kind == 'gpu_writer_bind_pending':
            state['pending'].append((address, size))
        elif kind in ('gpu_writer_compute_declared', 'gpu_writer_graphics_declared'):
            state['declared'].append(r)
        elif kind == 'writer_operation_recorded':
            for writer in state['declared']:
                writer = dict(writer, tick=max(n(writer, 'tick'), n(r, 'tick')))
                if n(writer, 'resource'):
                    state['buffers'][n(writer, 'resource')].assign(n(writer, 'address'), n(writer, 'address') + n(writer, 'bytes'), writer)
                else:
                    state['opaque'] = max(state['opaque'], n(writer, 'tick'))
            if state['opaque_pending']:
                state['opaque'] = max(state['opaque'], n(r, 'tick'))
            state['pending'].clear()
            state['declared'].clear()
            state['opaque_pending'] = False
        elif kind in ('gpu_writer_copy_exact', 'gpu_writer_fill_exact', 'gpu_writer_upload_exact',
                      'gpu_writer_image_download_conservative'):
            if handle and size:
                state['buffers'][handle].assign(address, address + size, r)
        elif kind == 'buffer_owner_retire':
            state['buffers'].pop(handle, None)
        elif kind == 'mapped_range_removed':
            for ranges in state['buffers'].values():
                ranges.remove(address, address + size)
        elif kind == 'readback_dependency_checkpoint':
            records = transactions[(sid, n(r, 'transaction'))]
            scopes = [e for e in records if e['kind'] in ('buffer_readback_sync', 'buffer_readback_async') and
                      n(e, 'begin_ns') <= n(r, 'begin_ns') <= n(e, 'end_ns')]
            if not scopes:
                continue
            scope = min(scopes, key=lambda e: n(e, 'end_ns') - n(e, 'begin_ns'))
            lo, hi = n(scope, 'begin_ns'), n(scope, 'end_ns')
            spans = [e for e in records if e['kind'] == 'readback_required_bytes' and lo <= n(e, 'begin_ns') <= hi]
            reasons, writers = [], []
            if len(recording_threads[sid]) > 1:
                reasons.append('multiple_recording_threads')
            if state['floor'] is None:
                reasons.append('missing_capture_floor')
            if not spans or sum((n(e, 'bytes') + 63) // 64 * 64 for e in spans) != n(scope, 'bytes'):
                reasons.append('incomplete_copy_footprint')
            if any(n(e, 'resource') != handle for e in spans):
                reasons.append('source_identity_mismatch')
            if state['opaque_pending']:
                reasons.append('opaque_operation_pending')
            for span in spans:
                a, length = n(span, 'address'), n(span, 'bytes')
                if any(intersects(a, length, b, other) for b, other in state['pending']):
                    reasons.append('overlapping_bound_write_pending')
                if any(intersects(a, length, n(e, 'address'), n(e, 'bytes')) for e in state['declared']):
                    reasons.append('declared_operation_pending')
                covered = state['buffers'][handle].covering(a, a + length)
                if covered is None:
                    reasons.append('missing_writer_coverage')
                else:
                    writers.extend(covered)
            known = max((n(e, 'tick') for e in writers), default=0)
            needed = max(known, state['opaque'], state['floor'] or 0)
            current, submitted = n(r, 'tick'), size
            if needed > submitted or needed >= current:
                reasons.append('required_tick_unsubmitted')
            observed_submit = submission.get((sid, needed))
            if observed_submit and n(observed_submit, 'submit_end_ns') > n(r, 'begin_ns'):
                reasons.append('submit_not_returned_at_checkpoint')
            waits = [e for e in records if e['kind'] == 'timeline_wait' and n(e, 'thread') == n(scope, 'thread') and
                     lo <= n(e, 'begin_ns') <= n(e, 'end_ns') <= hi]
            reasons = sorted(set(reasons))
            result.append(dict(scheduler=sid, transaction=n(r, 'transaction'), frame=n(r, 'frame'),
                guest_address=scope['resource'], source_buffer=hex(handle), copied_spans=';'.join(
                    f"{hex(n(e, 'address'))}+{n(e, 'bytes')}" for e in spans), packed_bytes=n(scope, 'bytes'),
                current_recording_tick=current, latest_submitted_tick=submitted,
                observed_range_writer_tick=known, opaque_writer_bound_tick=state['opaque'],
                capture_floor_tick=state['floor'], conservative_needed_tick=needed,
                earlier_than_queue_tail=not reasons and needed < submitted,
                dependency_bound_eligible=not reasons,
                intervening_submitted_batches=max(0, submitted - needed) if not reasons else 0,
                reasons=';'.join(reasons), writer_kinds=';'.join(sorted({e['kind'] for e in writers})),
                writer_sites=';'.join(sorted({f"{e.get('file', '')}:{e.get('line', '')}" for e in writers})),
                host_wait_ms=sum(n(e, 'end_ns') - n(e, 'begin_ns') for e in waits) / 1e6))
    return result


def writer_gpu_context(decisions, gpu):
    """Same-scheduler GPU-clock comparisons, never CPU wait savings estimates."""
    batches, copies, by_tick = {}, {}, defaultdict(list)
    for g in gpu:
        key = (n(g, '_scheduler'), n(g, 'tick'))
        by_tick[key].append(g)
        if g['kind'] == 'submit' and not n(g, 'split'):
            batches[key] = g
        if g['kind'] == 'copy_target_readback' and not n(g, 'split'):
            copies[(n(g, '_scheduler'), n(g, 'transaction'))] = g
    result = {}
    def summary(values):
        return dict(pairs=len(values), median_ms=statistics.median(values) if values else None,
                    max_ms=max(values, default=0), total_ms=sum(values))
    def end_to_start(writer, other, end=False):
        mask = n(writer, 'timestamp_mask')
        delta = (n(other, 'end_raw' if end else 'start_raw') - n(writer, 'end_raw')) & mask
        return delta * float(writer['timestamp_period_ns']) / 1e6 if delta < (mask + 1) // 2 else None
    for address in sorted({r['guest_address'] for r in decisions}):
        rows = [r for r in decisions if r['guest_address'] == address]
        gaps, tails, copy_ms, current_ms, writer_ms, detile_ms, compute_ms = [], [], [], [], [], [], []
        for r in rows:
            sid = r['scheduler']
            c = copies.get((sid, r['transaction']))
            b = batches.get((sid, r['current_recording_tick']))
            w = batches.get((sid, r['conservative_needed_tick']))
            t = batches.get((sid, r['latest_submitted_tick']))
            if c: copy_ms.append(float(c['gpu_ms']))
            if b: current_ms.append(float(b['gpu_ms']))
            if not r['dependency_bound_eligible']: continue
            if w:
                writer_ms.append(float(w['gpu_ms']))
                scopes = by_tick[(sid, r['conservative_needed_tick'])]
                detile_ms.append(sum(float(g['gpu_ms']) for g in scopes if g['kind'] == 'detile_dispatch'))
                compute_ms.append(sum(float(g['gpu_ms']) for g in scopes if g['kind'] == 'compute'))
            if w and c:
                value = end_to_start(w, c)
                if value is not None: gaps.append(value)
            if w and t:
                value = end_to_start(w, t, True)
                if value is not None: tails.append(value)
        result[address] = dict(calls=len(rows),
            intervening_batches_counts=dict(Counter(r['intervening_submitted_batches'] for r in rows)),
            source_buffers=sorted({r['source_buffer'] for r in rows}),
            needed_is_current_recording=sum(r['conservative_needed_tick'] == r['current_recording_tick'] for r in rows),
            copy_elapsed=summary(copy_ms), readback_batch_elapsed=summary(current_ms),
            writer_batch_end_to_copy_start=summary(gaps), writer_batch_end_to_tail_end=summary(tails),
            needed_writer_batch_elapsed=summary(writer_ms),
            needed_writer_batch_detile_scope_sum=summary(detile_ms),
            needed_writer_batch_compute_scope_sum=summary(compute_ms))
    return {'addresses': result,
        'limits': 'Same scheduler and GPU timestamp clock only. Required writer batch end is a conservative bound; '
                  'it can include unrelated operations after the actual writer. Stage elapsed includes stalls/preemption. '
                  'Scope sums can overlap. These differences do not measure CPU wait recoverability or transfer-queue concurrency.'}


def analyze_writers(events, submits, output, frames=(), gpu=()):
    rows = list(events.query("kind LIKE 'gpu_writer_%' OR kind LIKE 'readback_%' OR kind IN "
        "('writer_capture_floor','writer_operation_recorded','buffer_readback_sync','buffer_readback_async',"
        "'timeline_wait','buffer_owner_retire','mapped_range_removed')"))
    decisions = classify(rows, submits)
    dropped = sum(int(r['event_drops']) for r in frames)
    if dropped:
        for r in decisions:
            r['dependency_bound_eligible'] = r['earlier_than_queue_tail'] = False
            r['intervening_submitted_batches'] = 0
            r['reasons'] = ';'.join(filter(None, (r['reasons'], 'capture_event_drops')))
    with (output / 'readback-writer-bounds.csv').open('w', newline='', encoding='utf-8') as handle:
        if decisions:
            writer = csv.DictWriter(handle, decisions[0])
            writer.writeheader(); writer.writerows(decisions)
    reasons = Counter(reason for r in decisions for reason in r['reasons'].split(';') if reason)
    addresses = {}
    for r in decisions:
        item = addresses.setdefault(r['guest_address'], dict(calls=0, eligible=0, earlier_than_tail=0, total_wait_ms=0, earlier_bound_wait_ms=0))
        item['calls'] += 1; item['eligible'] += r['dependency_bound_eligible']; item['earlier_than_tail'] += r['earlier_than_queue_tail']
        item['total_wait_ms'] += r['host_wait_ms']
        if r['earlier_than_queue_tail']: item['earlier_bound_wait_ms'] += r['host_wait_ms']
    report = {'transactions': len(decisions), 'event_drops': dropped, 'reasons': dict(reasons), 'addresses': addresses,
        'limits': 'Eligibility is an observed conservative dependency bound, not proof of safe transfer-queue execution or recoverable wait. '
                  'Declared writers overapproximate stores; opaque BDA writes constrain all targets. Missing history blocks eligibility. '
                  'Queue-family sharing, aliases, CPU publication and future lifetime changes still need validation. Event drops invalidate coverage.'}
    (output / 'readback-writer-bounds.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (output / 'readback-writer-gpu-context.json').write_text(
        json.dumps(writer_gpu_context(decisions, gpu), indent=2), encoding='utf-8')
    return report
