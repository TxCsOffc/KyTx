import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('gpu_analysis',Path(__file__).with_name('analyze-gpuview.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class QueueAnalysisTests(unittest.TestCase):
    def test_qpc_boundaries_override_sequential_utc_reads(self):
        manifest={'emulator_id':10,'qpc_frequency':10000000,
                  'start':{'utc':'2026-10-06T00:00:00.002Z','qpc_ticks':100010000},
                  'end':{'utc':'2026-10-06T00:00:00.012Z','qpc_ticks':100100000}}
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'events.csv';path.write_text('')
            result,*_=m.analyze(path,manifest,'2026-10-06T00:00:00Z',qpc_zero_ticks=100000000)
        self.assertEqual(result['window_start_us'],1000)
        self.assertEqual(result['window_end_us'],10000)
        self.assertEqual(result['window_seconds'],.009)
        self.assertEqual(result['boundary_clock'],'qpc')

    def test_residence_is_not_execution_and_empty_gaps_are_conservative(self):
        # Packet queued at 2ms, reaches HW queue at 5ms, completes at 9ms.
        # An unrelated process occupies the same adapter in the initial 1ms gap.
        rows=[]
        def event(task,op,t,pid,fields):
            rows.append(['Microsoft-Windows-DxgKrnl/'+task+'/win:'+op,t,'test ('+str(pid)+')',7,0,'','','','']+
                        [k+' : '+str(v) for k,v in fields.items()])
        for pid,dev,ctx in ((10,'d1','c1'),(20,'d2','c2')):
            event('Device','DC_Start',0,99,{'hDevice':dev,'hProcessId':hex(pid),'pDxgAdapter':'gpu'})
            event('Context','DC_Start',0,99,{'hDevice':dev,'hContext':ctx,'NodeOrdinal':'0'})
        event('DmaPacket','Start',1000,99,{'hContext':'c2','PacketType':0,'uliSubmissionId':90,'ulQueueSubmitSequence':9})
        event('DmaPacket','Stop',2000,99,{'hContext':'c2','PacketType':0,'uliCompletionId':90,'ulQueueSubmitSequence':9,'bPreempted':'false'})
        event('QueuePacket','Start',2000,10,{'hContext':'c1','PacketType':0,'SubmitSequence':1})
        event('DmaPacket','Start',5000,99,{'hContext':'c1','PacketType':0,'uliSubmissionId':10,'ulQueueSubmitSequence':1})
        event('DmaPacket','Stop',9000,99,{'hContext':'c1','PacketType':0,'uliCompletionId':10,'ulQueueSubmitSequence':1,'bPreempted':'false'})
        event('QueuePacket','Stop',9000,99,{'hContext':'c1','PacketType':0,'SubmitSequence':1,'bTimeouted':'false','bPreempted':'false'})
        manifest={'emulator_id':10,'start':{'utc':'2026-10-06T00:00:00.001Z'},'end':{'utc':'2026-10-06T00:00:00.010Z'}}
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'events.csv'
            with path.open('w',newline='') as f:csv.writer(f).writerows(rows)
            result,*_=m.analyze(path,manifest,'2026-10-06T00:00:00Z')
        self.assertEqual(result['dma_residence_union_ms'],4)
        self.assertEqual(result['render_queue_residence_union_ms'],7)
        self.assertEqual(result['render_pending_without_kyty_dma_residence_ms'],3)
        self.assertEqual(result['no_kyty_render_or_dma_outstanding_ms'],2)
        self.assertEqual(result['other_adapter_dma_residence_during_kyty_empty_ms'],1)
        self.assertEqual(result['queue_latency_by_type_ms']['0']['max_ms'],7)
        self.assertEqual(result['dma_latency_by_type_ms']['0']['max_ms'],4)
        self.assertEqual(result['pairing_quality']['dma_start_without_stop'],0)

    def test_union_and_intersection_do_not_double_count_parallel_packets(self):
        a=m.union([(0,4),(2,8),(9,12)],1,10)
        self.assertEqual(a,[(1,8),(9,10)])
        self.assertEqual(m.duration(a),8)
        self.assertEqual(m.intersection(a,[(3,9)]),[(3,8)])

    def test_initial_unmatched_packet_retirement_bound_does_not_pollute_latency(self):
        def row(task,op,t,fields):
            return ['Microsoft-Windows-DxgKrnl/'+task+'/win:'+op,t,'test (10)',7,0,'','','','']+[k+' : '+str(v) for k,v in fields.items()]
        rows=[row('Device','DC_Start',0,{'hDevice':'d','hProcessId':'0xa','pDxgAdapter':'gpu'}),
              row('Context','DC_Start',0,{'hDevice':'d','hContext':'c','NodeOrdinal':'0'}),
              row('QueuePacket','Start',100,{'hContext':'c','PacketType':0,'SubmitSequence':1}),
              row('QueuePacket','Start',200,{'hContext':'c','PacketType':0,'SubmitSequence':2}),
              row('QueuePacket','Stop',900,{'hContext':'c','PacketType':0,'SubmitSequence':2,'bPreempted':'false'})]
        manifest={'emulator_id':10,'start':{'utc':'2026-10-06T00:00:00.001Z'},'end':{'utc':'2026-10-06T00:00:00.010Z'}}
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'events.csv'
            with path.open('w',newline='') as f:csv.writer(f).writerows(rows)
            result,*_=m.analyze(path,manifest,'2026-10-06T00:00:00Z')
        self.assertEqual(result['render_queue_residence_union_ms'],0)
        self.assertEqual(result['no_kyty_render_or_dma_outstanding_ms'],9)
        self.assertEqual(result['censored_queues'][0]['retirement_upper_bound'],900)
        self.assertEqual(result['queue_latency_by_type_ms']['0']['count'],0)

if __name__=='__main__':unittest.main()
