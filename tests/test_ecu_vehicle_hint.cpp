#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <Preferences.h>
#include "ecu_vehicle_hint.h"
#include "ecu_hint_store.h"
#include "diagnostic_policy.h"

using namespace m5can;
static DiagnosticRequest group(){
  DiagnosticRequest r{};r.can_id=0x18DB33F1U;r.length=3;
  r.data[0]=1;r.data[1]=0x0C;r.data[2]=0x0D;return r;
}
static DiagnosticResult capture() {
  DiagnosticResult r{};
  r.status=DiagnosticStatus::Ok;
  r.response_count=r.raw_frame_count=2;
  const uint32_t ids[]={0x18DAF102U,0x18DAF101U};
  for(unsigned i=0;i<2;++i){
    auto& m=r.responses[i];
    m.can_id=ids[i];m.length=6;
    const uint8_t p[]={0x41,0x0C,0x13,
                       static_cast<uint8_t>(0x88+i*4),0x0D,0x51};
    std::memcpy(m.data,p,6);
    auto& f=r.raw_frames[i];
    f.can_id=ids[i];f.dlc=8;f.data[0]=6;
    std::memcpy(f.data+1,p,6);
    r.raw_rx_offsets_us[i]=500+i*100;
  }
  return r;
}
int main(){
  Preferences::storage.clear();
  const auto sample=capture();
  const uint32_t preferred=0x18DAF101U;
  EcuVehicleHint record{};
  assert(EcuHintCodec::fromFunctional(sample,preferred,record));
  assert(EcuHintCodec::valid(record));
  assert(record.count==2 && record.source==preferred);
  assert(record.responders[0]==preferred);
  // Identical responder set irrespective of arrival order/value changes.
  auto reordered=sample;
  std::swap(reordered.responses[0],reordered.responses[1]);
  std::swap(reordered.raw_frames[0],reordered.raw_frames[1]);
  assert(EcuHintCodec::matches(record,reordered));
  auto changed=sample;
  changed.responses[0].data[3]^=4;
  changed.raw_frames[0].data[4]^=4;
  assert(EcuHintCodec::matches(record,changed));
  auto other_vehicle=sample;
  other_vehicle.responses[0].can_id=0x18DAF106U;
  other_vehicle.raw_frames[0].can_id=0x18DAF106U;
  assert(!EcuHintCodec::matches(record,other_vehicle));
  auto partial=sample;partial.responses[1].length=4;
  assert(!EcuHintCodec::matches(record,partial));
  auto broken_raw=sample;broken_raw.raw_frames[1].data[2]^=1;
  assert(!EcuHintCodec::matches(record,broken_raw));
  auto duplicate=sample;duplicate.responses[0]=duplicate.responses[1];
  duplicate.raw_frames[0]=duplicate.raw_frames[1];
  assert(!EcuHintCodec::matches(record,duplicate));
  auto corrupt=record;corrupt.responders[0]^=1;
  assert(!EcuHintCodec::valid(corrupt));
  corrupt=record;corrupt.version=2;
  assert(!EcuHintCodec::valid(corrupt));
  corrupt=record;corrupt.count=9;
  assert(!EcuHintCodec::valid(corrupt));

  // First fresh functional CAN reply must pass the CAN-owner authorization
  // gate before the saved ECU can be restored into the selector.
  EcuPhysicalSelector selector;
  DiagnosticRawFrame virtual_frames[2]{};
  assert(!selector.restoreAfterFunctionalProof(sample,preferred,virtual_frames));
  DiagnosticPolicy::observeFunctionalPairs(group(),sample);
  assert(selector.restoreAfterFunctionalProof(sample,preferred,virtual_frames));
  assert(selector.fixed() && selector.restoredNeedsPhysicalProof());
  assert(selector.physicalId()==0x18DA01F1U);

  // First physical reply missing: discard advisory selection immediately.
  auto timedout=sample;timedout.status=DiagnosticStatus::NoData;
  timedout.response_count=timedout.raw_frame_count=0;
  assert(!selector.observe(timedout,true,virtual_frames));
  assert(selector.state()==EcuPhysicalSelector::State::Discover);
  assert(selector.chosenResponse()==0);
  assert(!selector.restoredNeedsPhysicalProof());
  assert(selector.probes()==0);
  for(int i=0;i<8;++i)
    assert(selector.observe(sample,false,virtual_frames));
  assert(selector.fixed() && selector.probes()==8);

  // Flash is written only by an idle Arduino-loop poll, with readback.
  EcuHintStore first{};
  first.stage(record);
  assert(Preferences::writes==0);
  static CanMonitor can{};
  g_test_millis=1200;
  assert(first.poll(can));
  assert(Preferences::writes==1);
  assert(Preferences::storage.count("m5can_ecu/hint")==1);
  assert(!first.poll(can)); // at most one retained write
  EcuHintStore second{};
  second.loadBeforeBleStarts();
  EcuVehicleHint recovered{};
  assert(second.bootHint(recovered) && EcuHintCodec::valid(recovered));
  assert(EcuHintCodec::matches(recovered,sample));
  second.invalidateBootHint();
  assert(!second.bootHint(recovered));
  // A torn/corrupt NVS blob cannot be a boot-time authorization.
  Preferences::storage["m5can_ecu/hint"][4]^=0x80;
  EcuHintStore invalid{};
  invalid.loadBeforeBleStarts();
  assert(!invalid.bootHint(recovered));
  assert(Preferences::storage.size()==1);
  std::cout<<"PASS ECU hint CRC, full-set validation, stale re-probe, CAN proof and idle NVS\\n";
}
