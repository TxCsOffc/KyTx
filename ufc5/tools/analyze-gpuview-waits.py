"""Join switch-in saved stacks with preceding off-CPU intervals for one live thread.

These are off-CPU durations including ready time, not Vulkan API elapsed timings.
Symbols are RVA ranges exported by xperf's butterfly report for the same image.
"""
import argparse
import csv
import html
import json
import re
from collections import defaultdict, Counter
from pathlib import Path

def symbol_ranges(path):
    source=Path(path).read_text(encoding='utf-8-sig')
    table=source.split("<a id='TblSI'>",1)[1].split('</table>',1)[0]
    ranges=[]
    for row in re.findall(r'<tr[^>]*>(.*?)</tr>',table):
        cols=[html.unescape(re.sub('<[^>]*>','',x)) for x in re.findall(r'<td[^>]*>(.*?)</td>',row)]
        if len(cols)>=7 and cols[0].startswith('kyty_emulator'):
            ranges.append((int(cols[4],0),int(cols[5],0),cols[0].split('!',1)[1]))
    return ranges

def classify(symbols):
    joined=' '.join(symbols)
    if 'MasterSemaphore::Wait' in joined:
        return 'timeline_wait_readback' if 'DownloadBufferMemory' in joined else 'timeline_wait_other'
    if 'FreeDedicatedMemory' in joined:
        return 'driver_free_image' if 'Image::~Image' in joined else 'driver_free_other'
    if 'AllocateVulkanMemory' in joined or 'AllocateMemoryOfType' in joined:return 'driver_allocate_memory'
    if 'CommandScheduler::Submit' in joined:return 'driver_submit'
    if 'RunGarbageCollector' in joined:return 'garbage_collector_other'
    return 'other_stack'

def analyze(raw, report, tid, base, lo, hi, intervals_raw=None):
    ranges=symbol_ranges(report); stacks=defaultdict(list); intervals=[]; outgoing=None; last_switch=None
    with open(raw,encoding='utf-8-sig') as f:
        for line in f:
            event=line.split(',',1)[0].strip()
            if event=='CSwitch' and not intervals_raw:
                row=[x.strip() for x in line.split(',')]
                if row[1]=='TimeStamp':continue
                last_switch=int(row[1])
                if str(tid) not in (row[3],row[9]):continue
                t=int(row[1])
                if int(row[9])==tid:outgoing=(t,row[12],row[13])
                if int(row[3])==tid and outgoing:
                    a,state,reason=outgoing
                    if t>=a:intervals.append((a,t,state,reason))
                    outgoing=None
            elif event=='Stack':
                row=[x.strip() for x in line.split(',',5)]
                if row[1]=='TimeStamp' or int(row[2])!=tid:continue
                address=int(row[4],0); name=row[5]
                if name.startswith('kyty_emulator'):
                    rva=address-base
                    matches=[n for a,b,n in ranges if a<=rva<b or a==b==rva]
                    if matches:name=matches[0]
                stacks[int(row[1])].append(name)
    if intervals_raw:
        with open(intervals_raw,encoding='utf-8-sig') as f:
            for line in f:
                if line.split(',',1)[0].strip()!='CSwitch':continue
                row=[x.strip() for x in line.split(',')]
                if row[1]=='TimeStamp':continue
                last_switch=int(row[1])
                if str(tid) not in (row[3],row[9]):continue
                t=int(row[1])
                if int(row[9])==tid:outgoing=(t,row[12],row[13])
                if int(row[3])==tid and outgoing:
                    a,state,reason=outgoing
                    if t>=a:intervals.append((a,t,state,reason))
                    outgoing=None
    # A switch-out without a captured return is still known off CPU through the
    # selected end if global CSwitch coverage extends that far. Keep its cause
    # unknown: no saved return stack exists. Do not extend beyond trace coverage.
    tail=None
    if outgoing and last_switch is not None and last_switch>=hi:
        a,state,reason=outgoing
        if a<hi:
            tail=(a,hi,state,reason);intervals.append(tail)
    totals=defaultdict(lambda:{'count':0,'off_cpu_ms':0,'executive_off_cpu_ms':0,'max_ms':0})
    selected=[]; matched_start=matched_end=0
    for a,b,state,reason in intervals:
        if b<=lo or a>=hi:continue
        ms=(min(b,hi)-max(a,lo))/1000
        saved=stacks.get(b,[])
        matched_start+=bool(stacks.get(a)); matched_end+=bool(saved)
        censored=(a,b,state,reason)==tail
        category=classify(saved) if saved else ('missing_boundary_stack' if censored or b>hi or a<lo else 'missing_stack')
        group=totals[category];group['count']+=1;group['off_cpu_ms']+=ms;group['max_ms']=max(group['max_ms'],ms)
        if state=='Waiting' and reason=='Executive':group['executive_off_cpu_ms']+=ms
        selected.append({'begin_us':a,'end_us':b,'duration_ms':ms,'state':state,'reason':reason,'category':category,'switch_in_stack':saved,'censored':censored})
    return {'tid':tid,'image_base':hex(base),'window_start_us':lo,'window_end_us':hi,
            'categories':dict(totals),'intervals':selected,'switch_out_stack_matches':matched_start,'switch_in_stack_matches':matched_end,
            'limits':'Saved switch-in stack identifies the return path; off-CPU duration includes ready time. Full interval source, if provided, retains clipped boundaries and reports missing stacks. No GPU execution/API timing inferred.'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('raw');p.add_argument('--symbols',required=True)
    p.add_argument('--tid',type=int,required=True);p.add_argument('--image-base',type=lambda x:int(x,0),required=True)
    p.add_argument('--start-us',type=float,required=True);p.add_argument('--end-us',type=float,required=True);p.add_argument('--output',required=True);p.add_argument('--intervals-raw');a=p.parse_args()
    result=analyze(a.raw,a.symbols,a.tid,a.image_base,a.start_us,a.end_us,a.intervals_raw)
    Path(a.output).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='intervals'},indent=2))
