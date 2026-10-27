#include "can_monitor.h"
#include <cassert>
#include <iostream>
int main(){m5can::CanMonitor can;assert(can.begin());assert(!can.acquireLease(10000));assert(can.leaseRemainingMs()==0);m5can::DiagnosticRequest req{};req.can_id=0x18DB33F1;req.length=2;req.data[0]=1;req.data[1]=0x0c;assert(!can.submitQuery(req));assert(can.stats().tx_attempts==0&&can.stats().tx_success==0);std::cout<<"CAN hard lock: no driver, lease or submission PASS\n";}
