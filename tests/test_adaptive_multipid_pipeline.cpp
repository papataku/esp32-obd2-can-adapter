#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "adaptive_multi_pid.h"
#include "diagnostic_transaction.h"
#include "diagnostic_request_pacer.h"
#include "elm_compat.h"
#include "elm_response_formatter.h"

using namespace m5can;

static DiagnosticRequest convert(const ElmVehicleRequest& item) {
  DiagnosticRequest r{};
  r.can_id=item.can_id;
  r.length=item.length;
  for(uint8_t i=0;i<item.length;++i) r.data[i]=item.data[i];
  return r;
}
static TransactionFrame sf(uint32_t ecu, uint64_t ts,
                           uint8_t size, const uint8_t* payload) {
  TransactionFrame f{};
  f.can_id=ecu;
  f.timestamp_us=ts;
  f.extended=true;
  f.dlc=8;
  f.data[0]=size;
  for(uint8_t i=0;i<size;++i) f.data[i+1]=payload[i];
  return f;
}

int main() {
  ElmCompat elm;
  // Exercise the same existing meter request - no app protocol change.
  const ElmResult batch=elm.execute("ATM5B00:010C,010D,0105");
  assert(batch.action==ElmAction::BatchRead && batch.batch_count==3);
  const DiagnosticRequest rpm=convert(batch.batch[0]);
  const DiagnosticRequest speed=convert(batch.batch[1]);
  const DiagnosticRequest coolant=convert(batch.batch[2]);

  AdaptiveMultiPid shared_capability; // one object shared across two peers
  assert(shared_capability.eligible(rpm,speed));
  assert(!shared_capability.eligible(speed,coolant));
  const DiagnosticRequest grouped=AdaptiveMultiPid::group(rpm,speed);
  assert(grouped.can_id==0x18DB33F1U && grouped.length==3);
  assert(grouped.data[0]==0x01 && grouped.data[1]==0x0C &&
         grouped.data[2]==0x0D);
  assert(DiagnosticPolicy::allowedReadRequest(grouped));

  // The CAN owner wraps this 3-byte payload in a standard 8-byte SF:
  // 03 01 0C 0D 00 00 00 00.
  DiagnosticRequestPacer pacing;
  constexpr uint64_t sent_us=1000000ULL;
  assert(pacing.remainingUs(grouped,sent_us)==0);
  pacing.noteSuccessfulTx(grouped,sent_us);
  assert(pacing.remainingUs(rpm,sent_us+1000)==49000);
  assert(pacing.remainingUs(speed,sent_us+1000)==49000);
  assert(pacing.remainingUs(coolant,sent_us+1000)==0);

  // Simulated responding ECU: ISO-TP Single Frame with both requested
  // parameters; rpm=0x1388/4=1250, speed=0x50=80.
  DiagnosticTransaction transaction;
  transaction.begin(grouped,sent_us);
  const uint8_t other[]={0x41,0x05,0x67};
  assert(transaction.onFrame(sf(0x18DAF102U,sent_us+500,
                               sizeof(other),other)).event==
         TransactionEvent::Ignored);
  // A two-PID reply has six bytes total. An FF with a matching prefix
  // is NOT valid and must not trigger Flow Control on the vehicle bus.
  TransactionFrame unrelated_ff{};
  unrelated_ff.can_id=0x18DAF101U;
  unrelated_ff.extended=true;
  unrelated_ff.timestamp_us=sent_us+600;
  unrelated_ff.dlc=8;
  const uint8_t oversized[]={0x10,0x08,0x41,0x0C,0x13,0x88,0x0D,0x50};
  for(unsigned i=0;i<8;++i) unrelated_ff.data[i]=oversized[i];
  assert(transaction.onFrame(unrelated_ff).event==TransactionEvent::Ignored);
  assert(!transaction.hasActiveIsoTp());
  const uint8_t full[]={0x41,0x0C,0x13,0x88,0x0D,0x50};
  assert(transaction.onFrame(sf(0x18DAF101U,sent_us+800,
                               sizeof(full),full)).event==
         TransactionEvent::Completed);
  DiagnosticResult completed=transaction.result();
  completed.status=DiagnosticStatus::Ok;
  DiagnosticRawFrame virtual_frames[2]{};
  uint32_t responder=0;
  assert(shared_capability.evaluate(completed,virtual_frames,responder)==
         AdaptiveMultiPid::Decision::UseGrouped);
  assert(responder==0x18DAF101U);
  assert(shared_capability.eligible(rpm,speed));
  char buf[100]{};
  assert(ElmResponseFormatter::formatRawFrame(
             virtual_frames[0],true,true,buf,sizeof(buf)));
  assert(std::strcmp(buf,"18DAF101 04 41 0C 13 88")==0);
  assert(ElmResponseFormatter::formatRawFrame(
             virtual_frames[1],true,true,buf,sizeof(buf)));
  assert(std::strcmp(buf,"18DAF101 03 41 0D 50")==0);

  // ECU returns only RPM: reject incomplete grouping and fall back. A
  // second connected meter must not send another grouped probe afterward.
  DiagnosticTransaction incomplete;
  incomplete.begin(grouped,sent_us+100000);
  const uint8_t partial[]={0x41,0x0C,0x13,0x88};
  assert(incomplete.onFrame(sf(0x18DAF101U,sent_us+100500,
                              sizeof(partial),partial)).event==
         TransactionEvent::Completed);
  completed=incomplete.result();
  completed.status=DiagnosticStatus::Ok;
  assert(shared_capability.evaluate(completed,virtual_frames,responder)==
         AdaptiveMultiPid::Decision::RetryLegacy);
  assert(!shared_capability.eligible(rpm,speed));
  assert(DiagnosticPolicy::allowedReadRequest(rpm));
  assert(DiagnosticPolicy::allowedReadRequest(speed));

  // Other ECU(s) can respond only with the PIDs they own. This conservative
  // pilot does NOT combine cross-ECU replies into one artificial value.
  AdaptiveMultiPid split;
  DiagnosticTransaction split_tx;
  split_tx.begin(grouped,sent_us+200000);
  const uint8_t only_speed[]={0x41,0x0D,0x50};
  assert(split_tx.onFrame(sf(0x18DAF101U,sent_us+200500,
                              sizeof(partial),partial)).event==
         TransactionEvent::Completed);
  assert(split_tx.onFrame(sf(0x18DAF102U,sent_us+200800,
                              sizeof(only_speed),only_speed)).event==
         TransactionEvent::Completed);
  completed=split_tx.result();
  completed.status=DiagnosticStatus::Ok;
  assert(split.evaluate(completed,virtual_frames,responder)==
         AdaptiveMultiPid::Decision::RetryLegacy);
  assert(!split.eligible(rpm,speed));

  std::cout << "PASS ELM batch -> CAN multi-PID -> ISO-TP -> ELM replies -> fallback\\n";
  return 0;
}
