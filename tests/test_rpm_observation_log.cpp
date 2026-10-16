#include "rpm_observation_log.h"
#include <cassert>
#include <iostream>
using namespace m5can;

static RpmObservation rpm(uint64_t us,uint16_t value,uint64_t id,
                          RpmOrigin origin=RpmOrigin::FreshCan) {
  RpmObservation s{};
  s.observed_us=us; s.acquired_us=us-1000; s.acquisition_id=id;
  s.request_can_id=0x18DB33F1; s.service=1; s.pid=0x0C;
  s.outcome=RpmOutcome::Ok; s.origin=origin;
  s.physical_tx=(origin==RpmOrigin::FreshCan);
  s.responder_count=1;
  s.responders[0]={0x18DAF101,value,1,0};
  return s;
}
int main() {
  RpmRunCompressor log;
  // Representative Issue #31 interval, not the full private SD session.
  const uint64_t t0=593342000;
  const uint32_t dt[]={0,96000,206000,310000,420000,
                        530000,650000,796000,950000};
  for (unsigned i=0;i<9;++i)
    assert(log.append(rpm(t0+dt[i],470*4,i+1)));
  assert(log.size()==1);
  auto r=log.at(0);
  assert(r->observations==9 && r->physical_tx_count==9);
  assert(r->last_observed_us-r->first_observed_us==950000);
  assert(r->min_interval_us==96000 && r->max_interval_us==154000);
  assert(log.append(rpm(t0+1052000,0,10)));
  assert(log.size()==2 && log.at(1)->observations==1);

  auto c=rpm(t0+1090000,0,10,RpmOrigin::CanOwnerCache);
  c.acquired_us=t0+1051000;
  assert(log.append(c));
  c.observed_us+=20000; c.origin=RpmOrigin::CoordinatorCache;
  assert(log.append(c));
  r=log.at(1);
  assert(r->observations==3 && r->physical_tx_count==1);
  assert(r->by_origin[0]==1 && r->by_origin[1]==1 && r->by_origin[2]==1);
  assert(r->last_acquired_us==t0+1051000);

  RpmRunCompressor single,multiple;
  assert(single.append(rpm(100000,500*4,1)));
  assert(single.append(rpm(200000,0,2)));
  assert(single.at(0)->observations==1);
  assert(multiple.append(rpm(100000,500*4,1)));
  assert(multiple.append(rpm(150000,500*4,2)));
  assert(multiple.append(rpm(200000,0,3)));
  assert(multiple.at(0)->observations==2);
  auto no_data=rpm(210000,0,4);
  no_data.outcome=RpmOutcome::NoData;
  no_data.responder_count=0;
  assert(multiple.append(no_data));
  assert(multiple.size()==3 && multiple.at(2)->outcome==RpmOutcome::NoData);

  // Sorting ECU responses cannot alter the value-set identity.
  RpmRunCompressor ecu;
  auto x=rpm(100000,470*4,1);
  x.grouped=1; x.responder_count=2;
  x.responders[0]={0x18DAF102,470*4,1,0};
  x.responders[1]={0x18DAF101,470*4,1,0};
  assert(ecu.append(x));
  x.observed_us=200000; x.acquisition_id=2;
  x.responders[0]={0x18DAF101,470*4,1,0};
  x.responders[1]={0x18DAF102,470*4,1,0};
  assert(ecu.append(x) && ecu.size()==1);
  x.observed_us=300000; x.acquisition_id=3;
  x.responders[1].quarter_rpm=500*4;
  assert(ecu.append(x) && ecu.size()==2);
  x.observed_us=400000; x.responder_count=1;
  assert(ecu.append(x) && ecu.size()==3);
  x.observed_us=500000; x.responder_count=2;
  x.responders[1].can_id=x.responders[0].can_id;
  assert(!ecu.append(x) && ecu.stats().rejected==1);
  assert(ecu.stats().last_sequence==5);
  x.observed_us=600000; x.acquisition_id=4;
  x.responders[1].can_id=0x18DAF102;
  assert(ecu.append(x) && ecu.size()==4); // gap not silently compressed

  RpmRunCompressor wrap;
  for (uint64_t i=0;i<100;++i)
    assert(wrap.append(rpm((i+1)*100000,static_cast<uint16_t>(i*4),i+1)));
  assert(wrap.size()==64 && wrap.stats().overwritten_runs==36);
  assert(wrap.at(0)->first_sequence==37);
  auto regression=rpm(1,42,101);
  regression.acquired_us=0;
  assert(wrap.append(regression));
  assert(wrap.stats().clock_regressions==1);

  std::cout<<"PASS RPM run/provenance/ECU/gaps/counters\n";
}
