"""PR66 regression boundaries; no hardware access."""
import pathlib, sys, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import signal_v2_hil_ecu as h

class Review(unittest.TestCase):
    def test_m1_stopped_rx_drain(self):
        s = (ROOT/'src/twai_hil.cpp').read_text()
        self.assertNotIn('if(!started){vTaskDelay', s)
        self.assertIn('Stopped queues remain readable', s)
    def test_m2_empty_scenario_cannot_pass(self):
        for scenario in h.SCENARIOS:
            lines = ['#SIGNALV2_CAN,stopped=1,reason=RX_ACCOUNTING_OR_BUS_FAULT,invalid=1'] if h.SCENARIOS[scenario][0] else ['#SIGNALV2_ECU,physical_response=18DAF101']
            self.assertFalse(h.evaluate(scenario, [{'t': 0, 'kind':'line','text':l} for l in lines])['pass'], scenario)
    def test_m3_required_scenarios(self):
        self.assertTrue({'cf_probe','malformed','overflow','buserr','rearm','overlap','admission','disconnect','reboot'} <= set(h.SCENARIOS))
    def test_m4_board_time_required(self):
        self.assertFalse(h.evaluate('normal', [{'kind':'tx','t':0,'id':h.ECU_TX,'data':h.PROBE,'text':''}])['checks']['transport_complete'])
        s = (ROOT/'src/twai_hil.cpp').read_text()
        body = s.split('esp_err_t transmit(')[1].split('esp_err_t receive(')[0]
        self.assertNotIn('Serial.', body)
        self.assertIn('seq=', s)
    def test_m5_synchronized_admission(self):
        s = (ROOT/'src/twai_hil.cpp').read_text()
        self.assertIn('admitLocked', s)
        self.assertNotIn('xQueueSend', s) # no blocking queue API under spinlock
    def test_m6_headless_hil(self):
        s = (ROOT/'src/ui.cpp').read_text()
        self.assertIn('#if !defined(M5CAN_SIGNAL_V2_SERIAL_HIL)', s)
        self.assertIn('#else', s)
        self.assertIn('Headless HIL', s)
    def test_minor_driver_contract(self):
        s = (ROOT/'src/twai_hil.cpp').read_text()
        self.assertIn('g->rx_queue_len', s)
        self.assertIn('TWAI_STATE_BUS_OFF', s)
        self.assertNotIn('xQueueCreate(32', s)

if __name__ == '__main__': unittest.main()
