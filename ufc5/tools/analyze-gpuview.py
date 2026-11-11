"""Analyze field-named xperf GPUView CSV. Report queue residence, never shader time."""
import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

def union(intervals, lo, hi):
    out=[]
    for a,b in sorted((max(a,lo),min(b,hi)) for a,b in intervals if b>lo and a<hi):
        if b<=a: continue
        if out and a<=out[-1][1]: out[-1]=(out[-1][0],max(b,out[-1][1]))
        else: out.append((a,b))
    return out

def duration(intervals): return sum(b-a for a,b in intervals)

def intersection(a,b):
    i=j=0; out=[]
    while i<len(a) and j<len(b):
        x,y=max(a[i][0],b[j][0]),min(a[i][1],b[j][1])
        if y>x: out.append((x,y))
        if a[i][1]<b[j][1]: i+=1
        else: j+=1
    return out

def stats(values):
    v=sorted(values)
    if not v:return {'count':0}
    return {'count':len(v),'mean_ms':sum(v)/len(v)/1000,'p50_ms':v[len(v)//2]/1000,
            'p95_ms':v[min(len(v)-1,int(len(v)*.95))]/1000,'max_ms':v[-1]/1000}

def analyze(path, manifest, trace_start, thread_path=None, compact_path=None, qpc_zero_ticks=None):
    pid=manifest['emulator_id']; pidpat=re.compile(r'\(\s*'+str(pid)+r'\)')
    epoch=datetime.fromisoformat(trace_start.replace('Z','+00:00'))
    lo=(datetime.fromisoformat(manifest['start']['utc'].replace('Z','+00:00'))-epoch).total_seconds()*1e6
    hi=(datetime.fromisoformat(manifest['end']['utc'].replace('Z','+00:00'))-epoch).total_seconds()*1e6
    if qpc_zero_ticks is not None:
        lo=(manifest['start']['qpc_ticks']-qpc_zero_ticks)*1e6/manifest['qpc_frequency']
        hi=(manifest['end']['qpc_ticks']-qpc_zero_ticks)*1e6/manifest['qpc_frequency']
    devices={}; contexts={}; nodes={}; names={}; counts=Counter(); dma_open={}; queue_open={}
    dma=[]; queues=[]; unmatched=Counter(); thread_in={}; thread_out={}; cpu=defaultdict(list); offcpu=defaultdict(list)
    packets_threads=Counter(); present=[]; paging=Counter(); samples=Counter(); pair_errors=[]; runningtime=defaultdict(Counter)
    compact=open(compact_path,'w',encoding='utf-8') if compact_path else None
    with open(path,newline='',encoding='utf-8-sig',errors='strict') as f:
        for line in f:
            event=line.split(',',1)[0].strip()
            if event=='CSwitch':
                if not pidpat.search(line):continue
                row=[x.strip() for x in line.split(',')]
                if row[1]=='TimeStamp':continue
                t=int(row[1]); new=int(row[3]); old=int(row[9])
                if pidpat.search(row[8]):
                    if old in thread_in:cpu[old].append((thread_in.pop(old),t))
                    thread_out[old]=(t,row[12],row[13])
                if pidpat.search(row[2]):
                    if new in thread_out:
                        a,state,reason=thread_out.pop(new); offcpu[new].append((a,t,state,reason))
                    thread_in[new]=t
                continue
            if event=='ThreadName':
                if pidpat.search(line):
                    row=next(csv.reader([line],skipinitialspace=True))
                    if row[1].strip()!='TimeStamp':names[int(row[3])]=row[4].strip()
                continue
            if not event.startswith('Microsoft-Windows-DxgKrnl/'):continue
            task=event.split('/')[1]; opcode=event.split('/')[-1]
            if task not in ('Device','Context','NodeMetadata','DmaPacket','QueuePacket','PagingQueuePacket','Present','PresentHistory','UnwaitQueuePacket','UpdateContextRunningTime'):continue
            if compact:compact.write(line)
            row=[x.strip() for x in next(csv.reader([line],skipinitialspace=True))]
            if row[1]=='TimeStamp':continue
            t=int(row[1]); fields={x.split(' : ',1)[0]:x.split(' : ',1)[1] for x in row[9:] if ' : ' in x}
            if task=='Device' and opcode in ('win:DC_Start','win:Start'):
                devices[fields['hDevice']]={'pid':int(fields['hProcessId'],0),'adapter':fields['pDxgAdapter']}
            elif task=='Context' and opcode in ('win:DC_Start','win:Start'):
                contexts[fields['hContext']]={'device':fields['hDevice'],'node':fields['NodeOrdinal'],'parent':fields.get('ParentDxgContext')}
            elif task=='NodeMetadata':nodes[fields['pDxgAdapter']+'/'+fields['NodeOrdinal']]=fields
            elif task=='DmaPacket':
                key=(fields['hContext'],fields['PacketType'],fields.get('uliSubmissionId',fields.get('uliCompletionId')))
                if opcode=='win:Start':
                    if key in dma_open:unmatched['dma_duplicate_start']+=1
                    dma_open[key]=(t,fields)
                elif opcode=='win:Stop':
                    if key not in dma_open:
                        unmatched['dma_stop_without_start']+=1
                        pair_errors.append({'kind':'dma_stop_without_start','time':t,'fields':fields})
                    else:
                        a,start=dma_open.pop(key)
                        dma.append({'a':a,'b':t,'context':key[0],'type':int(key[1]),'id':key[2],
                                    'sequence':start['ulQueueSubmitSequence'],'preempted':fields.get('bPreempted')=='true'})
            elif task=='QueuePacket':
                key=(fields['hContext'],fields['SubmitSequence'])
                if opcode=='win:Start':
                    if key in queue_open:unmatched['queue_duplicate_start']+=1
                    queue_open[key]=(t,fields,row[2],int(row[3]))
                elif opcode=='win:Stop':
                    if key not in queue_open:
                        unmatched['queue_stop_without_start']+=1
                        pair_errors.append({'kind':'queue_stop_without_start','time':t,'fields':fields})
                    else:
                        a,start,process,tid=queue_open.pop(key)
                        queues.append({'a':a,'b':t,'context':key[0],'sequence':key[1],'type':int(fields['PacketType']),
                                       'process':process,'thread':tid,'start_fields':start,
                                       'preempted':fields.get('bPreempted')=='true','timeout':fields.get('bTimeouted')=='true'})
            elif task=='PagingQueuePacket' and opcode=='win:Start' and lo<=t<=hi:
                paging[row[2]]+=1
            elif task=='Present' and pidpat.search(row[2]) and lo<=t<=hi:present.append((t,opcode))
            elif task=='UpdateContextRunningTime' and lo<=t<=hi:
                runningtime[fields['hContext']][fields['Reason']]+=int(fields['RunningTime'])
    if compact:compact.close()
    if thread_path:
        with open(thread_path,encoding='utf-8-sig') as f:
            for line in f:
                event=line.split(',',1)[0].strip()
                if event=='ThreadName' and pidpat.search(line):
                    row=next(csv.reader([line],skipinitialspace=True))
                    if row[1].strip()!='TimeStamp':names[int(row[3])]=row[4].strip()
                elif event=='CSwitch' and pidpat.search(line):
                    row=[x.strip() for x in line.split(',')]
                    if row[1]=='TimeStamp':continue
                    t=int(row[1]); new=int(row[3]); old=int(row[9])
                    if pidpat.search(row[8]):
                        if old in thread_in:cpu[old].append((thread_in.pop(old),t))
                        thread_out[old]=(t,row[12],row[13])
                    if pidpat.search(row[2]):
                        if new in thread_out:
                            a,state,reason=thread_out.pop(new); offcpu[new].append((a,t,state,reason))
                        thread_in[new]=t
    unmatched['dma_start_without_stop']=len(dma_open); unmatched['queue_start_without_stop']=len(queue_open)
    def retirement_bound(records, ctx, sequence_field, sequence, start):
        # A later non-preempted render completion on this ordered context proves
        # an earlier render/wait retired by that time. Keep the bound explicit;
        # never include such a record in measured latency distributions.
        ends=[r['b'] for r in records if r['context']==ctx and r['type']==0 and not r.get('preempted')
              and not r.get('censored') and int(r[sequence_field])>int(sequence) and r['b']>=start]
        return min(ends) if ends else hi
    # Include boundary-truncated residence as censored intervals, but never in latency distributions.
    for (ctx,typ,ident),(a,fields) in dma_open.items():
        bound=retirement_bound(dma,ctx,'id',ident,a) if int(typ)==0 else hi
        dma.append({'a':a,'b':bound,'context':ctx,'type':int(typ),'id':ident,'sequence':fields['ulQueueSubmitSequence'],'censored':True,'retirement_upper_bound':bound if bound!=hi else None})
    for (ctx,seq),(a,fields,process,tid) in queue_open.items():
        typ=fields.get('PacketType')
        if typ is None:typ=4 if 'ObjectCount' in fields else 5
        bound=retirement_bound(queues,ctx,'sequence',seq,a)
        queues.append({'a':a,'b':bound,'context':ctx,'sequence':seq,'type':int(typ),'process':process,'thread':tid,'start_fields':fields,'censored':True,'preempted':False,'timeout':False,'retirement_upper_bound':bound if bound!=hi else None})
    def owner(ctx):
        c=contexts.get(ctx,{})
        d=devices.get(c.get('device'),{})
        return d.get('pid'),d.get('adapter'),c.get('node')
    kytycontexts={c:dict(info,**devices.get(info['device'],{})) for c,info in contexts.items() if owner(c)[0]==pid}
    adapters={owner(c)[1] for c in kytycontexts}
    kytydma=[d for d in dma if owner(d['context'])[0]==pid]
    kytyqueues=[q for q in queues if owner(q['context'])[0]==pid]
    for error in pair_errors:
        error['owner_pid']=owner(error['fields']['hContext'])[0]
    interior_errors=[e for e in pair_errors if lo<=e['time']<=hi]
    perowner=defaultdict(list)
    for d in dma:
        if owner(d['context'])[1] in adapters:perowner[owner(d['context'])[0]].append((d['a'],d['b']))
    dmau=union([(d['a'],d['b']) for d in kytydma],lo,hi)
    renderu=union([(q['a'],q['b']) for q in kytyqueues if q['type']==0],lo,hi)
    allu=union([i for xs in perowner.values() for i in xs],lo,hi)
    both=union(dmau+renderu,lo,hi)
    gap=[]; cursor=lo
    for a,b in both:
        if a>cursor:gap.append((cursor,a))
        cursor=max(cursor,b)
    if cursor<hi:gap.append((cursor,hi))
    for q in kytyqueues:
        if lo<=q['a']<=hi:packets_threads[q['thread']]+=1
    by_sequence=defaultdict(list)
    for d in kytydma:
        if not d.get('censored'):by_sequence[(d['context'],d['sequence'])].append(d)
    queue_phases=[]
    for q in kytyqueues:
        if q['type']!=0 or q.get('censored') or q['a']<lo or q['b']>hi:continue
        ds=by_sequence.get((q['context'],q['sequence']),[])
        if not ds:continue
        a=min(d['a'] for d in ds);b=max(d['b'] for d in ds)
        queue_phases.append({'context':q['context'],'sequence':q['sequence'],'enqueue_us':q['a'],'dma_enqueue_us':a,
                             'dma_stop_us':b,'queue_stop_us':q['b'],'native_queue_before_dma_us':a-q['a'],
                             'dma_residence_span_us':b-a,'queue_completion_after_dma_us':q['b']-b})
    phase_valid=[x for x in queue_phases if x['native_queue_before_dma_us']>=0 and x['queue_completion_after_dma_us']>=0]
    cpu_rows=[]
    for tid in set(cpu)|set(offcpu):
        running=duration(union(cpu[tid],lo,hi))
        states=defaultdict(float)
        for a,b,state,reason in offcpu[tid]:states[state+'/'+reason]+=max(0,min(b,hi)-max(a,lo))
        cpu_rows.append({'tid':tid,'name':names.get(tid,''),'running_ms':running/1000,'window_core_percent':100*running/(hi-lo),
                         'off_cpu_ms_by_switchout_state_reason':{k:v/1000 for k,v in sorted(states.items(),key=lambda x:-x[1])},
                         'queued_packets':packets_threads[tid],
                         'running_during_no_kyty_outstanding_ms':duration(intersection(union(cpu[tid],lo,hi),gap))/1000})
    result={'window_start_us':lo,'window_end_us':hi,'window_seconds':(hi-lo)/1e6,'pid':pid,
            'qpc_zero_ticks':qpc_zero_ticks,'boundary_clock':'qpc' if qpc_zero_ticks is not None else 'utc',
            'contexts':kytycontexts,'node_metadata':nodes,'pairing_quality':dict(unmatched),
            'running_time_raw_by_context_reason':{c:dict(v) for c,v in runningtime.items()},
            'pairing_errors_in_window':interior_errors,
            'censored_dma':[dict(d,owner_pid=owner(d['context'])[0]) for d in dma if d.get('censored')],
            'censored_queues':[dict(q,owner_pid=owner(q['context'])[0]) for q in queues if q.get('censored')],
            'dma_residence_union_ms':duration(dmau)/1000,'render_queue_residence_union_ms':duration(renderu)/1000,
            'adapter_any_dma_residence_union_ms':duration(allu)/1000,
            'render_pending_without_kyty_dma_residence_ms':(duration(renderu)-duration(intersection(renderu,dmau)))/1000,
            'no_kyty_render_or_dma_outstanding_ms':duration(gap)/1000,
            'no_kyty_outstanding_gap_stats':stats([b-a for a,b in gap]),
            'other_adapter_dma_residence_during_kyty_empty_ms':duration(intersection(gap,allu))/1000,
            'dma_residence_by_process_ms':{str(p):duration(union(v,lo,hi))/1000 for p,v in perowner.items()},
            'queue_latency_by_type_ms':{str(k):stats([q['b']-q['a'] for q in kytyqueues if q['type']==k and q['a']>=lo and q['b']<=hi and not q.get('censored')]) for k in sorted({q['type'] for q in kytyqueues})},
            'dma_latency_by_type_ms':{str(k):stats([d['b']-d['a'] for d in kytydma if d['type']==k and d['a']>=lo and d['b']<=hi and not d.get('censored')]) for k in sorted({d['type'] for d in kytydma})},
            'kyty_preempted_dma_stops':sum(d.get('preempted',False) for d in kytydma if lo<=d['b']<=hi),
            'kyty_queue_timeouts':sum(q['timeout'] for q in kytyqueues if lo<=q['b']<=hi),
            'matched_render_queue_phases':{
                'matched':len(queue_phases),'valid_nonnegative':len(phase_valid),
                'native_queue_before_dma':stats([x['native_queue_before_dma_us'] for x in phase_valid]),
                'dma_residence_span':stats([x['dma_residence_span_us'] for x in phase_valid]),
                'queue_completion_after_dma':stats([x['queue_completion_after_dma_us'] for x in phase_valid])},
            'paging_starts_by_emitting_process':dict(paging),'present_events':dict(Counter(op for t,op in present)),
            'cpu_threads':sorted(cpu_rows,key=lambda x:-x['running_ms']),
            'limits':['DMA residence includes hardware queue waiting, not pure GPU execution.',
                      'Queue completion/driver notifications lag execution; no-queue gaps are conservative.',
                      'Off-CPU intervals include time waiting plus later ready time; not all GPU waits.',
                      'Unmatched initial packets may use an explicit later in-context render-completion retirement upper bound.',
                      'Boundary unmatched events are retained as quality diagnostics; no per-resource/shader attribution.']}
    result['queue_phases']=queue_phases
    return result,kytydma,kytyqueues,gap

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('csv');p.add_argument('--manifest',required=True)
    p.add_argument('--trace-start',required=True);p.add_argument('--output',required=True);p.add_argument('--threads');p.add_argument('--compact');p.add_argument('--qpc-zero-ticks',type=int);a=p.parse_args()
    result,dma,queues,gaps=analyze(a.csv,json.loads(Path(a.manifest).read_text(encoding='utf-8-sig')),a.trace_start,a.threads,a.compact,a.qpc_zero_ticks)
    phases=result.pop('queue_phases')
    Path(a.output).write_text(json.dumps(result,indent=2),encoding='utf-8')
    Path(a.output+'.packets.json').write_text(json.dumps({'dma':dma,'queues':queues,'empty_gaps':gaps,'queue_phases':phases}),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('cpu_threads','node_metadata','contexts','pairing_errors_in_window','censored_dma','censored_queues','running_time_raw_by_context_reason')},indent=2))
    print('TOP CPU',json.dumps(result['cpu_threads'][:12],indent=2))
