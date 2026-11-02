// Included inside namespace m5can; lab macro only. Existing owner task is the
// sole caller; all TX still linearizes with STOP in transmitOwnedFrame.
void CanMonitor::performDiscovery11(const DiagnosticRequest& request,DiagnosticResult& result) {
  using D=signal_v2::Discovery11;

  auto fail=[&](const char* reason){
    discovery11_.failed=true;enterFaultLocked(reason);result.status=DiagnosticStatus::IsoTpError;
  };
  if(!request.signal_v2 || !D::validRequest(request.can_id,request.length,request.data) ||
     request.signal_v2_essential || request.timeout_ms!=D::ReceiveMs ||
     !leaseAllowsBudget(D::ObserveMs+50)) {fail("P5 request/lease");return;}
  // Reservations are retained across ARM. Never reset either rolling journal.
  if(!observed_budget_.permits(millis(),D::Frames) ||
     !signal_budget_.reserve(millis(),D::Frames,D::ObserveMs)) {fail("P5 lower budget");return;}
  hil::note("kind=p5_gate,gate=monitor,cost=3400,epoch=%lu",(unsigned long)request.authorization_epoch);
  if(!switchDriverMode(TWAI_MODE_NORMAL)){fail("P5 normal");return;}
  // Previous guard remains active through mode-switch and pre-TX drain.
  // Old queued replies are faults, never recycled into a fresh ARM.
  twai_message_t tx{};tx.identifier=0x7DF;tx.ss=1;tx.data_length_code=8;
  std::memcpy(tx.data,D::Request,8);
  if(!recordObservedDiagnostic(millis()) ||
     transmitOwnedFrame(tx,20,request.authorization_epoch)!=ESP_OK) {fail("P5 TX failure/epoch");}
  else {
    discovery11_=D{};discovery11_.guard=discovery11_.receiving=true;
    discovery11_.tx_us=stage_submit_us_;result.request_tx_us=stage_submit_us_;
    while(!fault_locked_) {
      // A consumed stage cannot acquire another lease; check current lease
      // and epoch directly, including asynchronous STOP/profile invalidation.
      if(request.authorization_epoch<=revoked_epoch_.load() ||
         request.authorization_epoch!=lease_epoch_.load() || !leaseAllowsBudget(1) ||
         uint32_t(millis()-discovery11_arm_ms_)>=D::DeadlineMs) {fail("P5 epoch/deadline");break;}
      twai_message_t rx{};
      const auto rc=twai_receive(&rx,pdMS_TO_TICKS(1));
      if(rc==ESP_OK)captureMessage(rx,static_cast<uint64_t>(esp_timer_get_time()));
      else if(rc!=ESP_ERR_TIMEOUT){fail("P5 receive");break;}
      updateDriverStats();
      if(fault_locked_)break;
      const uint64_t elapsed=static_cast<uint64_t>(esp_timer_get_time())-discovery11_.tx_us;
      if(elapsed>=D::ObserveMs*1000ULL) {
        twai_status_info_t state{};
        if(twai_get_status_info(&state)!=ESP_OK || state.msgs_to_rx)fail("P5 pending at close");
        break;
      }
    }
    discovery11_.receiving=false;
    if(!fault_locked_ && !discovery11_.failed && discovery11_.count) {
      result.status=DiagnosticStatus::Ok;
      result.completed_at_us=static_cast<uint64_t>(esp_timer_get_time());
      result.response_count=discovery11_.count;
      for(unsigned i=0;i<discovery11_.count;++i) {
        auto& out=result.responses[i];out.can_id=discovery11_.candidates[i].id;out.length=4;
        const auto bitmap=discovery11_.candidates[i].bitmap;
        for(unsigned j=0;j<4;++j)out.data[j]=uint8_t(bitmap>>(24-8*j));
      }
    } else fail("P5 no complete candidates");
  }
  revokeStageAuthorization(request.authorization_epoch);
  if(!switchDriverMode(TWAI_MODE_LISTEN_ONLY))fail("P5 listen-only");
}
