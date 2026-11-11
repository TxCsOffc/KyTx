import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('wait_analysis',Path(__file__).with_name('analyze-gpuview-waits.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class WaitAttributionTests(unittest.TestCase):
    def test_duration_ranking_uses_saved_return_stack_and_counts_missing(self):
        symbols="<a id='TblSI'><table>"
        for name,a,b in [('MasterSemaphore::Wait',0x100,0x110),('BufferCache::DownloadBufferMemory',0x200,0x210),('VmaAllocator_T::FreeDedicatedMemory',0x300,0x310),('Image::~Image',0x400,0x410)]:
            symbols+='<tr>'+''.join('<td>'+x+'</td>' for x in ['kyty_emulator.exe!'+name,'1','1%','0',hex(a),hex(b),'16'])+'</tr>'
        symbols+='</table>'
        rows=[]
        def switch(t,new,old):rows.append(['CSwitch',t,'test (10)',new,0,0,0,0,'test (10)',old,0,0,'Waiting','Executive'])
        def stack(t,address):rows.append(['Stack',t,7,1,hex(address),'kyty_emulator.exe!'+hex(address)])
        switch(100,8,7);switch(100100,7,8);stack(100100,0x140000105);stack(100100,0x140000205)
        switch(100200,8,7);switch(102200,7,8);stack(102200,0x140000305);stack(102200,0x140000405)
        switch(103000,8,7);switch(104000,7,8)
        with tempfile.TemporaryDirectory() as directory:
            raw=Path(directory)/'raw.csv';report=Path(directory)/'report.html'
            with raw.open('w',newline='') as f:csv.writer(f).writerows(rows)
            report.write_text(symbols)
            result=m.analyze(raw,report,7,0x140000000,0,200000)
        groups=result['categories']
        self.assertEqual(groups['timeline_wait_readback']['off_cpu_ms'],100)
        self.assertEqual(groups['driver_free_image']['off_cpu_ms'],2)
        self.assertEqual(groups['missing_stack']['off_cpu_ms'],1)
        self.assertEqual(sum(g['off_cpu_ms'] for g in groups.values()),103)
        self.assertEqual(result['switch_in_stack_matches'],2)

    def test_unreturned_wait_is_unknown_and_only_clipped_with_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            raw=Path(directory)/'raw.csv';report=Path(directory)/'report.html'
            report.write_text("<a id='TblSI'><table></table>")
            with raw.open('w',newline='') as f:
                csv.writer(f).writerows([
                    ['CSwitch',100,'test (10)',8,0,0,0,0,'test (10)',7,0,0,'Waiting','Executive'],
                    ['CSwitch',300,'test (10)',8,0,0,0,0,'test (10)',9,0,0,'Waiting','Executive']])
            result=m.analyze(raw,report,7,0x140000000,50,200)
            self.assertEqual(result['categories']['missing_boundary_stack']['off_cpu_ms'],.1)
            self.assertTrue(result['intervals'][0]['censored'])
            self.assertEqual(m.analyze(raw,report,7,0x140000000,50,400)['categories'],{})

if __name__=='__main__':unittest.main()
