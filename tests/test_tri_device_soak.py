"""Synthetic host parser cases; no physical-device acceptance."""
import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('tri_device_ble_soak',Path(__file__).resolve().parents[1]/'tools/tri_device_ble_soak.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class SoakParser(unittest.TestCase):
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
if __name__=='__main__':unittest.main()
