int main(int argc,char** argv){
  target=argc>1?unsigned(std::strtoul(argv[1],nullptr,10)):0;
  touch_stop=argc>2;
  m5can::CanMonitor m;CanOwnerTestAccess::setup(m);owner.attach(&m);
  owner.tick(0,{},false,1);
  connectSession(19,100);serviceAutoArm(100);control(4,2000);serviceAutoArm(2100);
  assert(can_armed);
  const uint32_t epoch=can_authorization_epoch.load();
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  owner.requestStageS1(epoch);
#endif
  g_test_millis=2100;tracking=true;
  Simulator::Demand demand{{200,200,200,200,200}};
  owner.tick(2100,demand,can_armed,epoch);
  if(has_queue)CanOwnerTestAccess::execute(m,queued);
  tracking=false;
  if(!target){
    assert(tx==1 && has_queue); // Positive control: current authorization really submits.
    std::cout<<"POINTS "<<events<<"\n";return 0;
  }
  assert(revoked && !can_armed);
  assert(after_revoke_tx==0);
#if !defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  assert(!owner.stopReason()); // Cancellation must not become an owner-live boot fault.
#endif
  assert(m.leaseRemainingMs()==0);
  assert(!CanOwnerTestAccess::acquire(m,epoch));
  // A later valid epoch must not lend its lease to an old queued request or FC.
  const uint32_t fresh=can_authorization_epoch.load()+1;
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  assert(m.beginStageS1(fresh));
#endif
  assert(CanOwnerTestAccess::acquire(m,fresh));
  const unsigned before=tx;
#ifdef PR70_EPOCH_API
  // Even a free request slot must reject an old capability at queue admission.
  CanOwnerTestAccess::clearPending(m);
  auto stale=queued;stale.signal_v2=true;stale.authorization_epoch=epoch;
  assert(!m.submitQuery(stale));
  assert(!m.acquireLease(550)); // Unbound V2 capabilities fail closed.
#endif
  assert(CanOwnerTestAccess::transmit(m,epoch,false)!=ESP_OK);
  assert(CanOwnerTestAccess::transmit(m,epoch,true)!=ESP_OK);
  if(has_queue)CanOwnerTestAccess::execute(m,queued);
  assert(tx==before && after_revoke_tx==0);
  std::cout<<"PASS "<<(touch_stop?"tap STOP":"disconnect")<<" point="<<target
           <<" post-revoke-TX="<<after_revoke_tx<<"\n";
}
