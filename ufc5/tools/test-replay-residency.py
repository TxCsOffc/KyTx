import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('residency', Path(__file__).with_name('analyze-replay-residency.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class NativeLifetimes(unittest.TestCase):
    def record(self, time, constructor=None):
        return dict(time_ns=time * 1000, memory='vk-memory', constructor=constructor,
                    thread=7, offset=0, extent=64, source='events', buffer='buffer')

    def allocation(self, index, begin, end, size=64):
        return dict(index=index, begin_us=begin, end_us=end, thread=7,
                    fields={'allocSize': str(size), 'PageTableOrDirectory': 'false'})

    def test_shared_block_and_retired_token_never_inherited(self):
        scope = dict(begin_ns='100000', end_ns='110000')
        records = [self.record(109, scope), self.record(150), self.record(201)]
        result = module.associate(records, [self.allocation(0, 105, 200)], 0, 10_000_000)
        self.assertEqual(result[0]['native_creation_association']['indices'], [0])
        self.assertEqual(result[1]['native_creation_association']['indices'], [0])
        self.assertIsNone(result[2]['native_creation_association'])

    def test_reused_token_rebinds_to_new_lifetime_and_preserves_multiple_allocations(self):
        records = [self.record(109, dict(begin_ns='100000', end_ns='110000')),
                   self.record(309, dict(begin_ns='300000', end_ns='310000'))]
        native = [self.allocation(0, 105, 200), self.allocation(1, 305, 400, 32),
                  self.allocation(2, 306, 400, 32)]
        result = module.associate(records, native, 0, 10_000_000)
        self.assertEqual(result[1]['native_creation_association']['indices'], [1, 2])

    def test_small_bookkeeping_allocation_does_not_define_backing_block(self):
        records = [self.record(109, dict(begin_ns='100000', end_ns='110000'))]
        result = module.associate(records, [self.allocation(0, 105, 200, 8)], 0, 10_000_000)
        self.assertIsNone(result[0]['native_creation_association'])

    def test_retired_side_allocation_makes_association_partial(self):
        records = [self.record(109, dict(begin_ns='100000', end_ns='110000')),
                   self.record(150)]
        native = [self.allocation(0, 105, 200), self.allocation(1, 106, 120, 8)]
        result = module.associate(records, native, 0, 10_000_000)
        association = result[1]['native_creation_association']
        self.assertEqual(association['status'], 'partial')
        self.assertEqual(association['live_indices'], [0])
        self.assertEqual(association['retired_indices'], [1])


if __name__ == '__main__':
    unittest.main()
