import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
import zlib

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('inspector', root/'tools/inspect_evidence_partition.py')
inspector = importlib.util.module_from_spec(spec); spec.loader.exec_module(inspector)


class InspectorTest(unittest.TestCase):
    def partition(self, payload):
        data = bytearray(b'\xff'*(4*4096))
        def header(magic, kind, sequence, slot, size, body=b''):
            first = inspector.HEADER.pack(magic, 1, kind, sequence, slot, size, 0, inspector.COMMIT)
            return inspector.HEADER.pack(magic, 1, kind, sequence, slot, size, zlib.crc32(first[:24]+body), inspector.COMMIT)
        data[:32] = header(inspector.OWNER, 0, 0, 0, len(data))
        data[8192:8224] = header(inspector.RECORD, 3, 1, 2, len(payload), payload)
        data[8224:8224+len(payload)] = payload
        return data

    def test_actual_cpp_layout_checksum_and_cut_context(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); source = path/'fixture.cpp'; binary = path/'fixture'
            source.write_text('''#include "ble_evidence_journal.h"
#include <iostream>
#include <cstring>
int main() {
 using J=m5can::BleEvidenceJournal; J::Record r;
 std::memset(&r,0,sizeof(r));r.magic=J::kMagic;r.version=J::kVersion;r.bytes=sizeof(r);
 r.boot=46;r.total=2;r.count=2;std::memset(r.build,'a',40);
 auto &e=r.events[0];e.seq=1;e.ms=1234;e.gen=2;e.conn=2;e.peer_suffix=0xdd41;
 e.mtu=247;e.reason=0;e.kind=3;e.reason_known=0;
 e.retries=100;e.failed=1;e.code=104;e.detail=6;e.rx_bytes=1000;e.tx_bytes=2000;
 r.events[1]=e;r.events[1].seq=2;r.events[1].ms=1300;r.events[1].kind=1;
 r.events[1].reason=534;r.events[1].reason_known=1;r.events[1].interval=12;
 r.events[1].supervision=60;r.events[1].code=0;r.events[1].detail=0;
 r.faults[0].code=104;r.faults[0].count=1;r.faults[0].first_detail=6;
 r.disconnect_peers[0].address_suffix=0xdd41;r.disconnect_peers[0].count=1;
 r.disconnect_peers[0].first_reason=534;
 uint32_t h=2166136261U;const auto *p=reinterpret_cast<const uint8_t*>(&r);
 for(size_t i=0;i<sizeof(r);++i)h=(h^p[i])*16777619U;r.crc=h;
 std::cout.write(reinterpret_cast<const char*>(&r),sizeof(r));
}''')
            subprocess.run(['g++', '-std=c++17', '-DM5CAN_DUAL_BLE_EXPERIMENTAL=1',
                '-I'+str(root/'tests/host_stubs'), '-I'+str(root/'src'), str(source), '-o', str(binary)], check=True)
            payload = subprocess.check_output([str(binary)])
            result = inspector.inspect(self.partition(payload))
            ble = result['records'][0]['ble']
            self.assertEqual(ble['events'][1]['reason_raw'], 534)
            self.assertEqual(ble['events'][0]['rx_bytes'], 1000)
            self.assertEqual(ble['faults'][0]['first_detail'], 6)
            self.assertEqual(ble['disconnect_peers'][0]['first_reason'], 534)
            self.assertEqual(result['ble_inner_invalid'], 0)
            data = self.partition(payload);data[8224+100] ^= 1
            self.assertEqual(inspector.inspect(data)['invalid_sectors'], 1)
            data[8192+28:8192+32] = b'\xff'*4
            self.assertEqual(inspector.inspect(data)['uncommitted_sectors'], 1)
            with self.assertRaises(ValueError): inspector.inspect(b'\xff'*len(data))


if __name__ == '__main__': unittest.main()
