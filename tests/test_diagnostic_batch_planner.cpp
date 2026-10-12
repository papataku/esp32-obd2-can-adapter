#include <cassert>
#include <iostream>
#include "diagnostic_batch_planner.h"
using namespace m5can;

static DiagnosticRequest req(uint8_t pid,uint32_t timeout=200) {
  DiagnosticRequest r{};
  r.can_id=0x18DB33F1U;r.length=2;r.data[0]=1;r.data[1]=pid;
  r.timeout_ms=timeout;
  return r;
}
int main() {
  const auto rpm=req(0x0C),speed=req(0x0D),coolant=req(0x05);
  AdaptiveMultiPid capability;
  using Kind=DiagnosticBatchPlanner::Kind;
  auto p=DiagnosticBatchPlanner::plan(rpm,nullptr,capability);
  assert(p.kind==Kind::Single && p.transmission.length==2);
  p=DiagnosticBatchPlanner::plan(rpm,&rpm,capability);
  assert(p.kind==Kind::JoinSameParameter &&
         p.transmission.data[1]==0x0C);
  p=DiagnosticBatchPlanner::plan(rpm,&speed,capability);
  assert(p.kind==Kind::GroupDistinct && p.transmission.length==3 &&
         p.transmission.data[1]==0x0C && p.transmission.data[2]==0x0D);
  p=DiagnosticBatchPlanner::plan(speed,&rpm,capability);
  assert(p.kind==Kind::GroupDistinct && p.transmission.length==3 &&
         p.transmission.data[1]==0x0C && p.transmission.data[2]==0x0D);
  p=DiagnosticBatchPlanner::plan(rpm,&coolant,capability);
  assert(p.kind==Kind::Single);
  auto mismatched=req(0x0D,400);
  p=DiagnosticBatchPlanner::plan(rpm,&mismatched,capability);
  assert(p.kind==Kind::Single); // Shorter timeout must not be extended
  capability.disable();
  p=DiagnosticBatchPlanner::plan(rpm,&speed,capability);
  assert(p.kind==Kind::Single); // One failed probe disables grouping
  p=DiagnosticBatchPlanner::plan(rpm,&rpm,capability);
  assert(p.kind==Kind::JoinSameParameter); // Coalescing is independent!
  std::cout<<"PASS pure CAN planner single/join/group, order, deadlines, "
             "disabled group independence\\n";
}
