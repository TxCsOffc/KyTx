"""Associate replay buffer bindings with lifetime-bounded native allocation events.

Creation-interval associations are evidence, not an exported Vulkan/driver handle
map. Keep one-to-many native allocations and unresolved/reused-block cases visible.
Reads existing production profiler and field-named GPUView exports only.
"""
import argparse
import csv
import json
from collections import Counter
from pathlib import Path


def buffer_records(profile):
    records = []
    for path in profile.parent.glob(profile.name + '*.events.csv'):
        with path.open(encoding='utf-8-sig', newline='') as stream:
            rows = list(csv.DictReader(stream))
        scopes = [r for r in rows if r['kind'] == 'buffer_native_allocate']
        for i, row in enumerate(rows):
            if row['kind'] not in ('buffer_allocation', 'detile_scratch_allocation') and not (
                    row['kind'].startswith('replay_') and row['kind'].endswith('_allocation')):
                continue
            companions = rows[i + 1:i + 5]
            if len(companions) != 4 or any(
                    r['begin_ns'] != row['begin_ns'] or r['thread'] != row['thread'] or
                    r['transaction'] != row['transaction'] for r in companions):
                raise ValueError('Incomplete allocation metadata: ' + str(path))
            meta = {r['kind']: r for r in companions}
            props, binding = meta['allocation_memory_properties'], meta['allocation_memory_binding']
            matches = [s for s in scopes if s['resource'] == row['resource'] and
                       int(s['begin_ns']) <= int(row['begin_ns']) <= int(s['end_ns'])]
            records.append(dict(
                source=str(path), role=row['kind'], buffer=row['resource'], size=int(row['bytes']),
                time_ns=int(row['begin_ns']), thread=int(row['thread']),
                memory=binding['resource'], offset=int(binding['bytes']),
                memory_properties=int(props['resource'], 0), memory_type=int(props['bytes']),
                extent=int(meta['allocation_memory_extent']['bytes']),
                mapped=meta['allocation_host_mapping']['resource'],
                usage=int(meta['allocation_host_mapping']['bytes']),
                constructor=matches[0] if len(matches) == 1 else None,
                constructor_matches=len(matches)))
    return sorted(records, key=lambda r: (r['time_ns'], r['role'] != 'buffer_allocation'))


def native_memory(path, pid):
    tasks = {'ReportSegment', 'AdapterAllocation', 'DeviceAllocation', 'PageInAllocation',
             'EvictAllocation', 'MigrateAllocation', 'PagingOpSysmemCommit',
             'PagingOpSysmemUncommit', 'PagingOpVirtualTransfer'}
    active, local, allocations, segments = {}, {}, [], {}
    with Path(path).open(encoding='utf-8-sig', newline='') as stream:
        for line in stream:
            name = line.split(',', 1)[0].split('/')
            if len(name) != 3 or name[1] not in tasks:
                continue
            row = [s.strip() for s in next(csv.reader([line], skipinitialspace=True))]
            if row[1] == 'TimeStamp':
                continue
            time, task, opcode = int(row[1]), name[1], name[2]
            f = {s.split(' : ', 1)[0]: s.split(' : ', 1)[1] for s in row[9:] if ' : ' in s}
            if task == 'ReportSegment':
                segments[f['pDxgAdapter'] + '/' + f['ulSegmentId']] = f
            elif task == 'AdapterAllocation':
                handle = f['hVidMmGlobalAlloc']
                if opcode == 'win:Start':
                    # Track all owners: pointers may be reused across processes.
                    old = active.get(handle)
                    if old is not None:
                        old['end_us'] = time
                        old['replaced_without_stop'] = True
                    item = dict(global_handle=handle, begin_us=time, end_us=None,
                                pid=int(f['hProcessId'], 0), thread=int(row[3]), fields=f, events=[])
                    active[handle] = item
                    if item['pid'] == pid:
                        item['index'] = len(allocations)
                        allocations.append(item)
                elif opcode == 'win:Stop':
                    item = active.pop(handle, None)
                    if item is not None:
                        item['end_us'] = time
            elif task == 'DeviceAllocation':
                handle = f['hVidMmAlloc']
                if opcode == 'win:Start':
                    item = active.get(f['hVidMmGlobalAlloc'])
                    local[handle] = item
                    if item and item['pid'] == pid:
                        item['events'].append(dict(time_us=time, task=task, opcode=opcode, fields=f))
                elif opcode == 'win:Stop':
                    local.pop(handle, None)
            else:
                if task == 'PageInAllocation':
                    item = local.get(f['hAllocationHandle'])
                else:
                    handle = f.get('hAllocationGlobalHandle', f.get('hGlobalAllocationHandle'))
                    item = active.get(handle)
                if item and item['pid'] == pid:
                    item['events'].append(dict(time_us=time, task=task, opcode=opcode, fields=f))
    return allocations, segments


def associate(records, allocations, origin, frequency):
    def cpu_us(ns):
        return int(ns) / 1000 - origin * 1e6 / frequency
    bindings = {}
    output = []
    for record in records:
        r = dict(record, time_us=cpu_us(record['time_ns']))
        memory, scope = r['memory'], r['constructor']
        created = []
        if scope:
            begin, end = cpu_us(scope['begin_ns']), cpu_us(scope['end_ns'])
            created = [a for a in allocations if a['thread'] == r['thread'] and
                       begin - 1 <= a['begin_us'] <= end + 1 and
                       a['fields'].get('PageTableOrDirectory') != 'true']
            r['constructor_us'] = [begin, end]
        # Do not mistake a small driver bookkeeping allocation inside vkCreateBuffer
        # for the backing memory block. This is a lower-bound sanity check, not
        # proof of native subrange coverage.
        native_bytes = sum(int(a['fields']['allocSize']) for a in created)
        r['native_created_indices'] = [a['index'] for a in created]
        if created and native_bytes >= r['offset'] + r['extent']:
            bindings[memory] = dict(indices=[a['index'] for a in created],
                                    creator=dict(source=r['source'], buffer=r['buffer'],
                                                 constructor_us=r['constructor_us']))
        binding = bindings.get(memory)
        live = [i for i in binding['indices'] if allocations[i]['end_us'] is None or
                allocations[i]['end_us'] >= r['time_us']] if binding else []
        valid = bool(live)
        if valid:
            r['native_creation_association'] = dict(binding, live_indices=live,
                status='complete' if len(live) == len(binding['indices']) else 'partial',
                retired_indices=[i for i in binding['indices'] if i not in live])
        else:
            r['native_creation_association'] = None
            r['unresolved_reason'] = 'No live native creation group for this Vulkan memory token'
        output.append(r)
    return output


def analyze(profile, dxg, correlated, origin, frequency=10_000_000):
    profile = Path(profile)
    correlation = json.loads(Path(correlated).read_text())
    pid = int(correlation['metadata']['replay_pid'])
    native, segments = native_memory(dxg, pid)
    records = associate(buffer_records(profile), native, origin, frequency)
    iterations = []
    for join in correlation['joins']:
        if join['warmup'] != '0' or join.get('native_match_count') != 1:
            continue
        source = join['submit']['scheduler'] + '.events.csv'
        phase = join.get('native_phase')
        if not phase:
            continue
        resources = []
        for role in ('replay_input_allocation', 'detile_scratch_allocation'):
            choices = [r for r in records if r['source'] == source and r['role'] == role and
                       r['time_us'] <= phase['dma_enqueue_us']]
            record = max(choices, key=lambda r: r['time_us']) if choices else None
            result = dict(role=role, record=record, native=[])
            if record and record['native_creation_association']:
                for index in record['native_creation_association']['live_indices']:
                    allocation = native[index]
                    states = [e for e in allocation['events'] if e['task'] == 'PageInAllocation'
                              and e['time_us'] <= phase['dma_enqueue_us']]
                    latest = max(states, key=lambda e: e['time_us']) if states else None
                    result['native'].append(dict(index=index,
                        alive_at_dma=allocation['end_us'] is None or allocation['end_us'] >= phase['dma_stop_us'],
                        page_in_before_dma=latest,
                        events_during_dma=[e for e in allocation['events']
                            if phase['dma_enqueue_us'] <= e['time_us'] <= phase['dma_stop_us']]))
            resources.append(result)
        iterations.append(dict(capture=join['capture'], iteration=join['iteration'],
                               gpu_ms=join['gpu_ms'], phase=phase, resources=resources))
    return dict(pid=pid, qpc_zero_ticks=origin, qpc_frequency=frequency,
                records=records, native_allocations=native, segments=segments, iterations=iterations,
                limits=['Native creation intervals are associations, not an exported Vulkan/native handle map.',
                        'One VkDeviceMemory may correspond to multiple native allocations; subrange mapping unknown.',
                        'Page-in is last observed placement; no complete per-page residency/physical-access proof.',
                        'No pure shader active time or whole-game FPS cause inferred.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile')
    parser.add_argument('dxg_csv')
    parser.add_argument('correlated_json')
    parser.add_argument('--qpc-zero-ticks', required=True, type=int)
    parser.add_argument('--qpc-frequency', default=10_000_000, type=int)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    result = analyze(args.profile, args.dxg_csv, args.correlated_json,
                     args.qpc_zero_ticks, args.qpc_frequency)
    Path(args.output).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(pid=result['pid'], records=len(result['records']),
                         native_allocations=len(result['native_allocations']),
                         measured_iterations=len(result['iterations']),
                         role_associations=dict(Counter(r['role'] for r in result['records']
                             if r['native_creation_association']))), indent=2))
