#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).resolve().parents[1]/'tools/analyze_two_meter_soak.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class AnalysisTest(unittest.TestCase):
    def test_boundaries_counter_reconnect_and_malformed_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'manifest.json').write_text(json.dumps({'measurement_start_monotonic': 10,
                'measurement_end_monotonic': 20, 'duration_s': 10, 'devices': {'ring': {}, 'jc': {}}}))
            data = [(9, 'adapter', '#BLE_DUAL,clients=2,connects=2,disconnects=99,rx_overflow=0,notify=0,retries=0,failed=0'),
                    (11, 'adapter', '#BLE_DUAL,clients=2,connects=2,disconnects=0,rx_overflow=0,notify=100,retries=0,failed=0'),
                    (19, 'adapter', '#BLE_DUAL,clients=2,connects=3,disconnects=1,rx_overflow=0,notify=200,retries=0,failed=0'),
                    (21, 'adapter', 'ESP-ROM: cleanup reset'),
                    (11, 'ring', 'M5CAN SIGNAL id=0 updates=10 age_ms=4 quality=1'),
                    (15, 'ring', 'M5CAN SIGNAL id=0 updates=bad'),
                    (19, 'ring', 'M5CAN SIGNAL id=0 updates=50 age_ms=5 quality=1'),
                    (19, 'ring', 'TACOLOG {"type":"saved","ok":false,"write_us":100}')]
            (root/'mac_events.jsonl').write_text(''.join(json.dumps({'monotonic': t, 'utc': str(t),
                'device': dev, 'line': line})+'\n' for t, dev, line in data))
            result = analysis.analyze(root)
            self.assertTrue(result['complete'])
            self.assertEqual(result['duration_s'], 10)
            self.assertEqual(result['reboots_or_panics'], [])
            self.assertEqual(result['counters']['adapter:disconnects']['delta'], 1)
            self.assertEqual(result['signals']['ring:0']['hz'], 5)
            self.assertEqual(result['malformed_rows']['ring:signal'], 1)
            self.assertEqual(result['ranges']['ring:save_failed']['max'], 1)
            self.assertIsNone(result['capture_coverage']['jc'])
            self.assertEqual(result['capture_coverage']['ring']['last_delay_s'], 1)


if __name__ == '__main__':
    unittest.main()
