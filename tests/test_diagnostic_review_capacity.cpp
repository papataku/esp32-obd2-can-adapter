#include "diagnostic_transaction.h"
#include <cassert>
using namespace m5can;
int main(){
  DiagnosticRequest req{};req.can_id=0x18DB33F1;req.length=2;
  req.data[0]=1;req.data[1]=0x0d;
  DiagnosticTransaction tx;tx.begin(req,1000);
  TransactionFrame f{};f.can_id=0x18DAF101;f.dlc=4;f.timestamp_us=2000;
  f.data[0]=3;f.data[1]=0x7f;f.data[2]=1;f.data[3]=0x78;
  for(unsigned i=0;i<kDiagnosticMaxRawFrames;++i)
    assert(tx.onFrame(f).event==TransactionEvent::ResponsePending);
  assert(tx.onFrame(f).event==TransactionEvent::ProtocolError);
  assert(tx.result().status==DiagnosticStatus::IsoTpError);
  tx.begin(req,1000);f.data[1]=0x41;f.data[2]=0x0d;f.data[3]=0;
  for(unsigned i=0;i<kDiagnosticMaxResponses;++i)
    assert(tx.onFrame(f).event==TransactionEvent::Completed);
  assert(tx.onFrame(f).event==TransactionEvent::ProtocolError);
  assert(tx.result().response_count==0 && tx.result().completed_at_us==0);
}
