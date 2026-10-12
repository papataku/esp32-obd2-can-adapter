#include <cassert>
#include <cstdint>
#include <iostream>
#include <initializer_list>

#include "adaptive_multi_pid.h"
#include "diagnostic_transaction.h"

using namespace m5can;

static DiagnosticRequest req(uint8_t pid) {
  DiagnosticRequest r{};
  r.can_id=0x18DB33F1U;
  r.length=2;
  r.data[0]=0x01;
  r.data[1]=pid;
  return r;
}

static DiagnosticResult reply(uint32_t can_id,
                              std::initializer_list<uint8_t> data) {
  DiagnosticResult r{};
  r.status=DiagnosticStatus::Ok;
  r.response_count=1;
  auto& m=r.responses[0];
  m.can_id=can_id;
  m.length=static_cast<uint16_t>(data.size());
  size_t i=0;
  for(uint8_t b : data) m.data[i++]=b;
  return r;
}

int main() {
  const auto rpm=req(0x0C),speed=req(0x0D);
  AdaptiveMultiPid cap;
  assert(cap.eligible(rpm,speed));
  assert(!cap.eligible(speed,rpm));
  const auto grouped=AdaptiveMultiPid::group(rpm,speed);
  assert(grouped.length==3 && grouped.data[0]==0x01 &&
         grouped.data[1]==0x0C && grouped.data[2]==0x0D);
  assert(DiagnosticPolicy::allowedReadRequest(grouped));
  const uint8_t partial[]={0x41,0x0C,0x10,0x20};
  assert(DiagnosticPolicy::responseMatches(grouped,partial,sizeof(partial)));

  DiagnosticRawFrame frames[2]{};
  uint32_t responder=0;
  const auto good=reply(0x18DAF101U,{0x41,0x0C,0x13,0x88,0x0D,0x50});
  assert(cap.decode(good,frames,responder));
  assert(responder==0x18DAF101U);
  assert(frames[0].data[0]==4 && frames[0].data[2]==0x0C &&
         frames[0].data[3]==0x13 && frames[0].data[4]==0x88);
  assert(frames[1].data[0]==3 && frames[1].data[2]==0x0D &&
         frames[1].data[3]==0x50);
  cap.markSupported(responder);
  assert(cap.support()==AdaptiveMultiPid::Support::Supported);
  assert(cap.decode(reply(0x18DAF101U,{0x41,0x0D,0x60,0x0C,0x10,0x00}),
                    frames,responder)); // reply need not match PID order
  assert(frames[0].data[3]==0x10 && frames[1].data[3]==0x60);
  assert(!cap.decode(reply(0x18DAF102U,{0x41,0x0C,1,2,0x0D,3}),
                     frames,responder));
  assert(!cap.decode(reply(0x18DAF101U,{0x41,0x0C,1,2}),frames,responder));
  assert(!cap.decode(reply(0x18DAF101U,{0x41,0x0C,1,2,0x0C,3}),
                     frames,responder));
  assert(!cap.decode(reply(0x18DAF101U,{0x7F,0x01,0x12}),frames,responder));
  auto timedout=good;
  timedout.status=DiagnosticStatus::NoData;
  assert(!cap.decode(timedout,frames,responder));
  cap.disable();
  assert(!cap.eligible(rpm,speed));

  // Ensure a grouped Single Frame reaches the ISO-TP transaction layer
  // but an unrelated mode or CAN responder does not.
  DiagnosticTransaction t;
  t.begin(grouped,1000);
  TransactionFrame wrong{};
  wrong.extended=true;
  wrong.timestamp_us=1100;
  wrong.can_id=0x18DAF102U;
  wrong.dlc=8;
  wrong.data[0]=0x04;
  wrong.data[1]=0x42; // unexpected service, ignored
  wrong.data[2]=0x0C;
  assert(t.onFrame(wrong).event==TransactionEvent::Ignored);
  TransactionFrame sf{};
  sf.extended=true;
  sf.timestamp_us=1200;
  sf.can_id=0x18DAF101U;
  sf.dlc=8;
  const uint8_t bytes[]={6,0x41,0x0C,0x13,0x88,0x0D,0x50,0};
  for(unsigned i=0;i<8;++i)sf.data[i]=bytes[i];
  assert(t.onFrame(sf).event==TransactionEvent::Completed);
  assert(t.result().response_count==1);
  responder=0;
  AdaptiveMultiPid fresh;
  DiagnosticResult completed=t.result();
  completed.status=DiagnosticStatus::Ok;
  assert(fresh.decode(completed,frames,responder));

  // Decision policy: unsupported/incomplete read returns to the legacy
  // per-PID path and does not allow another auto-probe in the same boot.
  {
    AdaptiveMultiPid probe;
    const auto decision=probe.evaluate(good,frames,responder);
    assert(decision==AdaptiveMultiPid::Decision::UseGrouped);
    assert(probe.support()==AdaptiveMultiPid::Support::Supported);
    const auto missing=reply(0x18DAF101U,{0x41,0x0C,1,2});
    assert(probe.evaluate(missing,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(probe.support()==AdaptiveMultiPid::Support::Disabled);
    assert(!probe.eligible(rpm,speed));
  }
  {
    AdaptiveMultiPid rejected;
    const auto nrc=reply(0x18DAF101U,{0x7F,0x01,0x12});
    assert(rejected.evaluate(nrc,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(!rejected.eligible(rpm,speed));
  }
  for (const DiagnosticStatus status :
       {DiagnosticStatus::NoData,DiagnosticStatus::IsoTpError}) {
    AdaptiveMultiPid probe;
    auto failed=good;
    failed.status=status;
    assert(probe.evaluate(failed,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(!probe.eligible(rpm,speed));
  }
  // Fault/lease/policy errors must not initiate extra CAN transactions.
  for (const DiagnosticStatus status :
       {DiagnosticStatus::LeaseRequired,DiagnosticStatus::BusOff,
        DiagnosticStatus::ModeError,DiagnosticStatus::DriverError,
        DiagnosticStatus::InvalidRequest,DiagnosticStatus::Busy,
        DiagnosticStatus::RateLimited}) {
    AdaptiveMultiPid probe;
    auto failed=good;
    failed.status=status;
    assert(probe.evaluate(failed,frames,responder)==
           AdaptiveMultiPid::Decision::Stop);
    assert(!probe.eligible(rpm,speed));
  }
  // A functional request can receive responses from several ECUs. Do not
  // combine disjoint RPM and speed data from different responder IDs.
  {
    AdaptiveMultiPid probe;
    auto two_ecus=reply(0x18DAF101U,{0x41,0x0C,0x12,0x34});
    two_ecus.responses[1]=reply(
        0x18DAF102U,{0x41,0x0D,0x55}).responses[0];
    two_ecus.response_count=2;
    assert(probe.evaluate(two_ecus,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
  }
  // A FULL valid pair from ECU01 accompanied by another ECU's valid
  // partial response is STILL ambiguous. Never select the first result.
  {
    AdaptiveMultiPid probe;
    auto mixed=good;
    mixed.responses[1]=reply(
        0x18DAF102U,{0x41,0x0D,0x50}).responses[0];
    mixed.response_count=2;
    assert(!probe.decode(mixed,frames,responder));
    assert(probe.evaluate(mixed,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(!probe.eligible(rpm,speed));
  }
  // Actual 2026-10-12 raw examples: 18DAF1EF and 18DAF10E both
  // supplied the complete '41 0C 00 00 0D 00' payload. The other three
  // replies were NOT preserved. These additional three entries are SYNTHETIC,
  // not claimed as actual vehicle observations.
  {
    AdaptiveMultiPid probe;
    auto many=reply(0x18DAF1EFU,{0x41,0x0C,0,0,0x0D,0});
    const uint32_t ids[]={0x18DAF10EU,0x18DAF105U,0x18DAF1F0U,0x18DAF101U};
    for(unsigned i=0;i<4;++i)
      many.responses[i+1]=reply(ids[i],{0x41,0x0C,0,0,0x0D,0}).responses[0];
    many.response_count=5;
    assert(probe.evaluate(many,frames,responder)==
           AdaptiveMultiPid::Decision::UseGrouped);
    assert(responder==0x18DAF101U);
    assert(frames[0].can_id==responder && frames[1].can_id==responder);
    assert(frames[0].data[3]==0 && frames[1].data[3]==0);
    assert(probe.lastRejectReason()==AdaptiveMultiPid::RejectReason::None);

    // Subsequent grouped reads must stay pinned to the chosen ECU even
    // when additional ECU responses differ from the chosen ECU.
    many.responses[0]=reply(0x18DAF1EFU,
        {0x41,0x0C,0x30,0x30,0x0D,0x40}).responses[0];
    many.responses[1]=reply(0x18DAF10EU,
        {0x41,0x0D,0x70}).responses[0]; // extraneous partial ECU
    assert(probe.evaluate(many,frames,responder)==
           AdaptiveMultiPid::Decision::UseGrouped);
    assert(responder==0x18DAF101U);

    // The pinned responder may not disappear and be replaced by another,
    // even if that other ECU provides a fully valid pair.
    many.responses[4]=reply(0x18DAF103U,
        {0x41,0x0C,0,0,0x0D,0}).responses[0];
    assert(probe.evaluate(many,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(probe.lastRejectReason()==
           AdaptiveMultiPid::RejectReason::LearnedEcuMissing);
    assert(responder==0 && !probe.eligible(rpm,speed));
  }
  // Unproven ECU mixes must NEVER be accepted merely because one response
  // happens to be complete. Arrival order does not select the winner.
  {
    AdaptiveMultiPid probe;
    auto differing=good;
    differing.responses[1]=reply(0x18DAF102U,
        {0x41,0x0C,0x13,0x89,0x0D,0x50}).responses[0];
    differing.response_count=2;
    assert(probe.evaluate(differing,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(probe.lastRejectReason()==
           AdaptiveMultiPid::RejectReason::ValuesDisagree);
    assert(responder==0 && !probe.eligible(rpm,speed));
  }
  {
    AdaptiveMultiPid probe;
    auto duplicates=good;
    duplicates.responses[1]=good.responses[0];
    duplicates.response_count=2;
    assert(probe.evaluate(duplicates,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(probe.lastRejectReason()==
           AdaptiveMultiPid::RejectReason::DuplicateEcu);
  }
  {
    AdaptiveMultiPid probe;
    auto incomplete=good;
    incomplete.responses[1]=reply(0x18DAF102U,
        {0x41,0x0D,0x50}).responses[0];
    incomplete.response_count=2;
    assert(probe.evaluate(incomplete,frames,responder)==
           AdaptiveMultiPid::Decision::RetryLegacy);
    assert(probe.lastRejectReason()==
           AdaptiveMultiPid::RejectReason::InvalidPair);
    assert(!probe.eligible(rpm,speed));
  }
  {
    AdaptiveMultiPid probe;
    auto all=good;
    all.response_count=static_cast<uint8_t>(kDiagnosticMaxResponses+1);
    assert(!probe.decode(all,frames,responder)); // bounded array guard
    assert(responder==0);
  }
  // Reject a response with an unexpected extra PID, not only truncation.
  assert(!fresh.decode(reply(0x18DAF101U,
      {0x41,0x0C,0x12,0x34,0x0D,0x60,0x05,0x44}),frames,responder));

  std::cout << "PASS bounded automatic multi-PID probe and response validation\n";
  return 0;
}
