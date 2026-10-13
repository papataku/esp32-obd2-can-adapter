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
                    (11, 'ring', 'M5CAN SIGNAL id=0 value_x100=10 quality=1 updates=10 age_ms=4'),
                    (15, 'ring', 'M5CAN SIGNAL id=0 updates=bad'),
                    (16, 'ring', 'M5CAN SIGNAL id=0 value_x100=10 quality=1 updates=40 99 age_ms=4'),
                    (19, 'ring', 'M5CAN SIGNAL id=0 value_x100=10 quality=1 updates=50 age_ms=5'),
                    (19, 'ring', 'TACOLOG {"type":"saved","ok":false,"write_us":100}')]
            (root/'mac_events.jsonl').write_text(''.join(json.dumps({'monotonic': t, 'utc': str(t),
                'device': dev, 'line': line})+'\n' for t, dev, line in data))
            result = analysis.analyze(root)
            self.assertTrue(result['complete'])
            self.assertEqual(result['duration_s'], 10)
            self.assertEqual(result['reboots_or_panics'], [])
            self.assertEqual(result['counters']['adapter:disconnects']['delta'], 1)
            self.assertEqual(result['signals']['ring:0']['hz'], 5)
            self.assertEqual(result['malformed_rows']['ring:signal'], 2)
            self.assertEqual(result['ranges']['ring:save_failed']['max'], 1)
            self.assertIsNone(result['capture_coverage']['jc'])
            self.assertEqual(result['capture_coverage']['ring']['last_delay_s'], 1)

    def test_ui_policy_change_is_not_a_transport_rate_regression(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'manifest.json').write_text(json.dumps({'measurement_start_monotonic':10,
                'measurement_end_monotonic':20,'duration_s':10,'devices':{'jc':{}},'meter_bench_preset':'high'}))
            lines=[(10,'M5CAN DISPLAY_POLL mask=1 auto=0 background=2 foreground=20/20/20/1/1'),
                   (10,'M5CAN SIGNAL id=0 value_x100=10 quality=1 updates=100 age_ms=1'),
                   (12,'M5CAN DISPLAY_POLL mask=1 auto=0 background=2 foreground=20/20/20/1/1'),
                   (12,'M5CAN SIGNAL id=0 value_x100=10 quality=1 updates=120 age_ms=1'),
                   (13,'M5CAN DISPLAY_POLL mask=1 auto=0 background=2 foreground=20/20/20/1/1FPS'),
                   (15,'M5CAN DISPLAY_POLL mask=2 auto=1 background=2 foreground=20/20/20/1/1'),
                   (20,'M5CAN SIGNAL id=0 value_x100=10 quality=1 updates=136 age_ms=1')]
            (root/'mac_events.jsonl').write_text(''.join(json.dumps({'monotonic':t,'utc':str(t),'device':'jc','line':line})+'\n' for t,line in lines))
            result=analysis.analyze(root)
            self.assertEqual(result['signals']['jc:0']['hz'],3.6)
            policy=result['rate_policies']['jc']
            self.assertEqual(policy['initial_matched_high_rates']['0']['hz'],10)
            self.assertEqual(policy['initial_matched_high_s'],2)
            self.assertEqual(policy['changes'][-1]['auto'],1)
            self.assertEqual(result['malformed_rows']['jc:policy'],1)


if __name__ == '__main__':
    unittest.main()
