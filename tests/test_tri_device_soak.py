"""Synthetic host parser cases; no physical-device acceptance."""
import importlib.util
from pathlib import Path
import unittest
import json
import tempfile
spec=importlib.util.spec_from_file_location('tri_device_ble_soak',Path(__file__).resolve().parents[1]/'tools/tri_device_ble_soak.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class SoakParser(unittest.TestCase):
 def test_bench_measurement_requires_applied_rates(self):
  self.assertTrue(module.bench_applied('M5CAN BENCH RATE APPLIED rpm=20 speed=20 power=20 coolant=1 soc=1','high'))
  self.assertFalse(module.bench_applied('M5CAN BENCH RATE REQUEST rpm=20 speed=20 power=20 coolant=1 soc=1 queued=1','high'))
  self.assertFalse(module.bench_applied('M5CAN BENCH RATE APPLIED rpm=20 speed=5 power=20 coolant=1 soc=1','high'))
  self.assertFalse(module.bench_applied('M5CAN BENCH RATE APPLIED rpm=20 speed=20 power=20 coolant=2 soc=1','high'))
 def test_disconnect_keeps_reason_epoch_and_unknown(self):
  rows=[{'utc':'2026-10-12T00:00:00Z','device':'adapter','line':'#BLE_EVENT,kind=DISCONNECT,seq=2,ms=1900,id=7,gen=3,addr=ABCD,reason_known=1,reason=520,drops=0'}, {'utc':'2026-10-12T00:00:01Z','device':'ring','line':'M5CAN SIGNAL id=0 value_x100=1200 updates=15 age_ms=42'}]
  out=module.analyze(rows)
  self.assertEqual(out['disconnects'][0]['reason'],'520')
  self.assertEqual(out['disconnects'][0]['gen'],'3')
  self.assertEqual(out['last_signals']['ring:0']['age_ms'],'42')
  self.assertEqual(out['reboots'],[])
 def test_restart_and_partial_log_are_not_healthy(self):
  out=module.analyze([{'utc':'x','device':'ring','line':'ESP-ROM:esp32s3'}, {'utc':'x','device':'adapter','line':'#BLE_DUAL,clients=1,failed=2'}])
  self.assertEqual(out['reboots'][0]['device'],'ring')
  self.assertEqual(out['notify_failures'],2)
  self.assertEqual(out['last_peers'],{})
class EvidenceWindow(unittest.TestCase):
 def test_missing_capture_never_becomes_full_physical_pass(self):
  spec=importlib.util.spec_from_file_location('analyze',Path(__file__).resolve().parents[1]/'tools/analyze_tri_device_soak.py')
  analyzer=importlib.util.module_from_spec(spec);spec.loader.exec_module(analyzer)
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory)
   rows=[{'utc':'2026-10-12T00:00:02Z','device':'ring','line':'M5CAN SIGNAL id=0 updates=9 age_ms=30 quality=1'}, {'utc':'2026-10-12T00:00:03Z','device':'ring','line':'M5CAN SIGNAL id=0 updates=10 age_ms=ms=253 quality=1'}]
   (p/'mac_events.jsonl').write_text('\n'.join(json.dumps(x) for x in rows)+'\n')
   app=[{'utc':'2026-10-12T00:00:00Z','kind':'DEMO_VERIFIED','note':'MODE=DEMO'}, {'utc':'2026-10-12T00:00:01Z','kind':'SAMPLE','note':'latency_ms=60'}, {'utc':'2026-10-12T00:00:30Z','kind':'SOAK_END','note':'failed=false'}]
   (p/'app.jsonl').write_text('\n'.join(json.dumps(x) for x in app))
   result=analyzer.run_analysis(p,p/'app.jsonl')
   self.assertTrue(result['app_complete'])
   self.assertEqual(result['samples'],1)
   self.assertEqual(result['malformed_signal_rows'],1)
   self.assertEqual(result['latency_ms']['p95'],60)
   self.assertIsNone(result['signals']['ring:0']['actual_hz'])
   self.assertFalse(result['full_host_coverage'])
   self.assertNotIn('physical_pass',result)
if __name__=='__main__':unittest.main()
