namespace signal_v2 {
inline void CanOwner::tickDiscovery11(uint32_t now,bool armed,uint32_t epoch) {
  using D=Discovery11;
  // This image cannot reach the regular acquisition branch, even with demand.
  if(!discovery11_active_) {
    if(in_flight_){m5can::DiagnosticResult stale{};if(can_->pollQueryResult(stale))in_flight_=false;}
    can_->revokeLease();return;
  }
  if(!armed || epoch!=discovery11_epoch_ || epoch<=revoked_epoch_.load() ||
     uint32_t(now-discovery11_start_)>=D::DeadlineMs || can_->faultLocked()) {
    stop("P5_EPOCH_DEADLINE_FAULT");discovery11_active_=false;return;
  }
  if(in_flight_) {
    m5can::DiagnosticResult r{};
    if(!can_->pollQueryResult(r))return;
    in_flight_=false;
    if(r.status==m5can::DiagnosticStatus::Ok && r.request_tx_us && r.completed_at_us &&
       r.response_count && r.response_count<=D::ResponderLimit) {
      // Stable ascending ID order, only complete current-epoch candidates.
      for(unsigned id=0x7E8;id<=0x7EF;++id)for(unsigned i=0;i<r.response_count;++i) {
        const auto& c=r.responses[i];
        if(c.can_id==id && c.length==4)
          Serial.printf("#P5,Learning=1,LIVE=0,epoch=%lu,id=%03X,bitmap=%02X%02X%02X%02X\n",
            (unsigned long)epoch,id,c.data[0],c.data[1],c.data[2],c.data[3]);
      }
      stop("P5_COMPLETE",6);
    } else stop("P5_RESULT_FAULT");
    discovery11_active_=false;return;
  }
  if(!budget_.reserve(now,D::Frames,D::ObserveMs)) {
    stop("P5_OWNER_BUDGET");discovery11_active_=false;return;
  }
  hil::note("kind=p5_gate,gate=owner,cost=3400,epoch=%lu",(unsigned long)epoch);
  m5can::DiagnosticRequest q{};q.signal_v2=true;q.authorization_epoch=epoch;
  q.can_id=0x7DF;q.length=2;q.data[0]=1;q.data[1]=0;q.timeout_ms=D::ReceiveMs;
  if(!can_->acquireLease(D::DeadlineMs-uint32_t(now-discovery11_start_),epoch) || !can_->submitQuery(q)) {
    stop("P5_ADMISSION_FAULT");discovery11_active_=false;return;
  }
  submitted_epoch_=epoch;in_flight_=true;
}
} // namespace signal_v2
