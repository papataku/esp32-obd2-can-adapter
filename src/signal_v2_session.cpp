#include "signal_v2_session.h"
#include "signal_v2_demo.h"
#include <algorithm>
#include <cstring>
namespace signal_v2 {
constexpr uint16_t Simulator::Pids[5];
void Session::connect(uint32_t boot, uint32_t session) {
  for (auto &h : history_)
    h.used = false;
  cursor_ = 0;
  high_ = 0;
  boot_ = boot;
  id_ = session;
  revision_ = deadline_ = next_ = frame_ = sample_ = 0;
  hello_ = false;
  paused_ = true;
  count_ = cache_count_ = 0;
  cache_size_ = 0;
  period_ = 200;
  snapshot_ = false;
  snapshot_request_id_ = 0;
  observed_ms_ = 0;
  plan_size_ = 0;
  plan_id_ = 0;
  plan_deadline_ = 0;
  cache_lengths_.fill(0);
  due_.fill(0);
  heartbeat_ = 0;
}
void Session::ack(const Packet &req, Result result, const uint8_t *body,
                  size_t n, Packet &out) {
  uint8_t p[486]{};
  auto q = req.payload();
  w32(p, u32(q));
  p[4] = q[8];
  p[5] = result;
  w32(p + 8, revision_);
  w16(p + 12, n);
  if (n)
    std::memcpy(p + 16, body, n);
  out = encode(0x80, boot_, hello_ ? id_ : 0, u32(req.bytes.data() + 16), p,
               16 + n);
}
void Session::control(const Packet &req, bool authorized, uint32_t now,
                      Packet &out) {
  out.size = 0;
  if (req.size < 38 || req.bytes[4] != 1)
    return;
  const auto p = req.payload();
  const uint32_t rid = u32(p), expected = u32(p + 4);
  const uint8_t op = p[8];
  const size_t n = u16(p + 10);
  const auto b = p + 12;
  if (!authorized) {
    invalidate();
    ack(req, NotAuthorized, nullptr, 0, out);
    return;
  }
  if (!rid) {
    ack(req, ProtocolError, nullptr, 0, out);
    return;
  }
  if ((op == 1 && (!hello_) &&
       (u32(req.bytes.data() + 8) || u32(req.bytes.data() + 12))) ||
      (op != 1 && (!hello_ || u32(req.bytes.data() + 8) != boot_ ||
                   u32(req.bytes.data() + 12) != id_)) ||
      (op == 1 && hello_ &&
       (u32(req.bytes.data() + 8) || u32(req.bytes.data() + 12)))) {
    ack(req, ProtocolError, nullptr, 0, out);
    return;
  }
  for (const auto &h : history_)
    if (h.used && u32(h.request.payload()) == rid) {
      if (h.request.size == req.size &&
          std::equal(p, p + n + 12, h.request.payload()))
        out = h.ack;
      else
        ack(req, ProtocolError, nullptr, 0, out);
      return;
    }
  if (rid <= high_) {
    ack(req, ResultUnknown, nullptr, 0, out);
    return;
  }
  Result result = Accept;
  // RELAXED carries the complete 8+24*N proposed profile, up to N=16.
  uint8_t reply[MaxPdu - Header - 16]{};
  size_t reply_n = 0;
  if (expected != revision_)
    result = BadRevision;
  else
    switch (op) {
    case 1:
      if (hello_ || n != 12 || b[0] != 1 || b[1] != 2 || b[2] || b[3] ||
          u16(b + 4) < 50)
        result = ProtocolError;
      else {
        hello_ = true;
        w16(reply, 512);
        w16(reply + 2, 512);
        w16(reply + 4, 244);
        w16(reply + 6, 3000);
        reply[8] = static_cast<uint8_t>(MaxSubscriptions);
        reply[10] = 1;
        reply[11] = 0;
        w32(reply + 12, 1);
        reply_n = 16;
      }
      break;
    case 2: {
      if (n < 8 || b[4] > MaxSubscriptions || b[5] || b[6] || b[7] ||
          n != size_t(8 + 24 * b[4]) || revision_ == UINT32_MAX ||
          u32(b) != revision_ + 1) {
        result = Reject;
        break;
      }
      // All accepted profiles must fit a COMPLETE snapshot, never truncate it.
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
      size_t snapshot_bytes = 20; // correlated CAN Snapshot prefix
#else
      size_t snapshot_bytes = 16;
#endif
      uint8_t rejection_reason = 0;
      bool valid = true, relax = false;
      for (unsigned i = 0; i < b[4]; ++i) {
        const auto e = b + 8 + 24 * i;
        uint16_t pid = u16(e + 4);
        snapshot_bytes += 44 + (pid == 0x9a ? 8 : (pid == 0x0c ? 4 : 3));
        valid &= u16(e) != 0 && e[2] == 1 && e[3] == 0 &&
                 (pid == 0x0c || pid == 0x0d || pid == 0x05 || pid == 0x5b ||
                  pid == 0x9a) &&
                 e[6] <= 3 && e[7] == 0 &&
                 // Priority 0 is reserved exclusively for speed (Service 01 PID 0D).
                 ((pid == 0x0d) == (e[6] == 0)) &&
                 u32(e + 12) == 1 &&
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
                 u32(e + 8) == 0 &&
#endif
                 u16(e + 16) >= 1 && u16(e + 16) <= 10000 &&
                 u16(e + 18) >= u16(e + 16) && u16(e + 20) >= u16(e + 16) &&
                 e[22] == 0 && e[23] == 0;
        if (u16(e + 16) <
#if defined(M5CAN_SIGNAL_V2_10HZ_LAB)
            100
#else
            200
#endif
            ) {
          relax = true;
          valid &= u16(e + 18) >= 200 && u16(e + 20) >= 200;
        }
        for (unsigned j = 0; j < i; ++j)
          if (u16(e) == u16(b + 8 + 24 * j))
            valid = false;
      }
      if (snapshot_bytes > MaxPdu - Header) {
        valid = false;
        rejection_reason = 1; // SNAPSHOT_TOO_LARGE
      }
      if (!valid) {
        result = Reject;
        reply[0] = rejection_reason ? rejection_reason : 2; // PROFILE_INVALID
        reply_n = 1;
        break;
      }
      if (relax) {
        plan_id_ = rid;
        plan_deadline_ = now + 3000;
        plan_size_ = n;
        std::copy(b, b + n, plan_.begin());
        for (unsigned i = 0; i < b[4]; ++i) {
          auto e = plan_.data() + 8 + 24 * i;
          if (u16(e + 16) < 200)
            w16(e + 16, 200);
        }
        result = Relaxed;
        w32(reply, plan_id_);
        std::copy(plan_.begin(), plan_.begin() + plan_size_, reply + 4);
        reply_n = 4 + plan_size_;
        break;
      }
      apply(b, now);
      w32(reply, revision_);
      w16(reply + 4, period_);
      w16(reply + 6, 600);
      reply_n = 8;
      break;
    }
    case 3:
      if (n != 8 || !plan_size_ || u32(b) != plan_id_ ||
          u32(b + 4) != u32(plan_.data()) ||
          int32_t(plan_deadline_ - now) <= 0 || u32(b + 4) != revision_ + 1) {
        result = Reject;
        break;
      }
      apply(plan_.data(), now);
      w32(reply, revision_);
      w16(reply + 4, period_);
      w16(reply + 6, 600);
      reply_n = 8;
      break;
    case 4:
      if (n)
        result = ProtocolError;
      else if (!active(now))
        result = TxLocked;
      else
        deadline_ = now + 3000;
      break;
    case 5:
    case 6:
      if (n != 4 || revision_ == UINT32_MAX || u32(b) != revision_ + 1)
        result = ProtocolError;
      else if (op == 6 && (int32_t(deadline_ - now) <= 0 || !count_))
        result = TxLocked;
      else {
        revision_ = u32(b);
        paused_ = op == 5;
        cache_count_ = 0;
        cache_size_ = 0;
        cache_lengths_.fill(0);
        due_.fill(now);
        snapshot_ = op == 6;
        next_ = now;
      }
      break;
    case 7:
      if (n != 8)
        result = ProtocolError;
      else if (u32(b + 4) != revision_)
        result = BadRevision;
      else if (!active(now))
        result = TxLocked; // expired/paused leases cannot revive valid cache
      else {
        snapshot_request_id_ = rid;
        snapshot_ = true;
      }
      break;
    case 8:
      if (n)
        result = ProtocolError;
      else {
        w32(reply, revision_);
        w32(reply + 4, reservations(now));
        reply_n = 8;
      }
      break;
    case 9:
      if (n != 8)
        result = ProtocolError;
      else {
        std::copy(b, b + 8, reply);
        w64(reply + 8, uint64_t(now) * 1000);
        reply_n = 16;
      }
      break;
    case 10:
      if (n)
        result = ProtocolError;
      else {
        paused_ = true;
        deadline_ = 0;
        count_ = 0;
      }
      break;
    default:
      result = Unsupported;
      break;
    }
  ack(req, result, reply, reply_n, out);
  auto &h = history_[cursor_++ % history_.size()];
  h.used = true;
  h.request = req;
  h.ack = out;
  high_ = rid;
}
void Session::demand(uint32_t now, Simulator::Demand &result) const {
  if (!active(now))
    return;
  for (unsigned i = 0; i < count_; ++i) {
    auto e = entries_.data() + 24 * i;
    for (unsigned j = 0; j < 5; ++j)
      if (u16(e + 4) == Simulator::Pids[j]) {
        uint16_t period = u16(e + 16);
        if (!result[j] || period < result[j])
          result[j] = period;
      }
  }
}
void Session::apply(const uint8_t *b, uint32_t now) {
  count_ = b[4];
  std::copy(b + 8, b + 8 + 24 * count_, entries_.begin());
  revision_ = u32(b);
  period_ = 10000;
  for (unsigned i = 0; i < count_; ++i)
    period_ = std::min(period_, u16(b + 8 + 24 * i + 16));
  deadline_ = now + 3000;
  paused_ = false;
  next_ = now;
  snapshot_ = true;
  snapshot_request_id_ = 0;
  cache_count_ = 0;
  cache_size_ = 0;
  cache_lengths_.fill(0);
  due_.fill(now);
  plan_size_ = 0;
}
bool Session::produce(uint32_t now, Packet &out, const Simulator *source, bool demo_enabled, const CanSamples *can) {
  if (!hello_)
    return false;
  // Expiration invalidates pending snapshots as well as demand. Never reissue
  // a VALID cache after the acquisition lease expired or PAUSE was applied.
  if (!active(now)) {
    snapshot_ = false;
    cache_count_ = 0;
    cache_size_ = 0;
    cache_lengths_.fill(0);
  }
  uint8_t p[486]{};
  if (snapshot_) {
    snapshot_ = false;
    // Snapshot cannot refresh old CAN data. Remove any stale/invalid cache.
    if(can)for(unsigned i=0;i<count_;++i){
      const auto e=entries_.data()+24*i;
      const auto c=can->sample(u16(e+4));
      const uint32_t last_seq=cache_lengths_[i]?
          u32(cache_.data()+52*i+38):0;
      if(!c||c->quality>1||!c->observed_us||
         uint64_t(now)*1000ULL < c->observed_us||
         last_seq!=c->sequence||
         u32(cache_.data()+52*i+14)!=can->source_generation||
         u32(cache_.data()+52*i+18)!=can->mode_epoch||
         uint64_t(now)*1000ULL>c->observed_us+
              uint64_t(u16(e+20))*1000ULL)cache_lengths_[i]=0;
    }
    w32(p, frame_ + 2);
    w32(p + 4, 3);
    w32(p + 8, revision_);
    size_t pos = can ? 20 : 16;
    if(can){p[13]=2;w32(p+16,snapshot_request_id_);}
    for (unsigned i = 0; i < count_; ++i)
      if (cache_lengths_[i]) {
        ++p[12];
        std::copy(cache_.data() + 52 * i,
                  cache_.data() + 52 * i + cache_lengths_[i], p + pos);
        if(can){
          const uint64_t observed=uint64_t(u32(p+pos+30))<<32|u32(p+pos+26);
          const uint64_t age=uint64_t(now)*1000ULL-observed;
          p[pos+2]=2;
          w32(p+pos+34,age>UINT32_MAX?UINT32_MAX:static_cast<uint32_t>(age));
        }
        pos += cache_lengths_[i];
      }
    out = encode(0x21, boot_, id_, ++frame_, p, pos);
    return true;
  }
  if (!active(now) || int32_t(now - next_) < 0) {
    if (uint32_t(now - heartbeat_) < 1000)
      return false;
    heartbeat_ = now;
    w32(p, revision_);
    w32(p + 8, 1);
    out = encode(0x22, boot_, id_, ++frame_, p, 12);
    return true;
  }
  size_t pos = 2;
  next_ = now + 10000;
  for (unsigned i = 0; i < count_; ++i) {
    auto e = entries_.data() + 24 * i;
    if (int32_t(now - due_[i]) >= 0) {
      const uint16_t period = u16(e + 16);
      // Keep the admitted phase across transient ACK/notify backpressure.
      // Skip missed deadlines, never manufacture old observations or bursts.
      due_[i] += (uint32_t(now - due_[i]) / period + 1) * period;
      uint16_t pid = u16(e + 4);
      auto r = cache_.data() + 52 * i;
      uint8_t len = pid == 0x9a ? 8 : (pid == 0x0c ? 4 : 3);
      size_t size = 44 + len;
      std::fill(r, r + 52, 0);
      w16(r, size);
      r[2] = can ? 2 : 1;
      r[3] = can ? 1 : 2;
      w16(r + 4, u16(e));
      r[6] = 1;
      r[7] = pid == 0x0c ? 1 : 0;
      w16(r + 8, pid);
      w32(r + 10, revision_);
      w32(r + 14, can ? can->source_generation : 1);
      w32(r + 18, can ? can->mode_epoch : 1);
      w32(r + 22, can ? can->source_id : u32(e + 8));
      const auto shared = source ? source->sample(pid) : nullptr;
      const auto actual = can ? can->sample(pid) : nullptr;
      // Tests without an external source exercise the original active
      // synthetic path. Physical V2 lab always provides a Simulator.
      const bool available = can
          ? (actual && actual->quality<=1 && actual->length==len &&
             actual->observed_us &&
             uint64_t(now)*1000ULL >= actual->observed_us &&
             uint64_t(now)*1000ULL <= actual->observed_us +
                  uint64_t(u16(e+20))*1000ULL)
          : demo_enabled && (!source || shared);
      const uint32_t sample_sequence = !available ? 0 :
          (can ? actual->sequence : (shared ? shared->sequence : ++sample_));
      size_t actual_len=0;
      if(available&&can){
        actual_len=actual->length;
        std::copy(actual->raw,actual->raw+actual_len,r+44);
      }else if(available){
        actual_len=demoPayload(pid,sample_sequence,r+44);
      }
      if (available && actual_len != len) return false;
      len = static_cast<uint8_t>(actual_len);
      size = 44 + len;
      w16(r, static_cast<uint16_t>(size));
      r[7] = !available ? (can ? 8 : 2)
            : (can ? actual->quality : (pid == 0x0d && r[46] == 0 ? 1 : 0));
      w64(r + 26, available ?
          (can ? actual->observed_us :
                 uint64_t(shared ? shared->observed_ms : now)*1000) : 0);
      // CAN v2 record: acquisition age at this PDU's encoding time. DEMO
      // retains its legacy uncertainty field and wire fixtures unchanged.
      w32(r + 34, available ? (can ? static_cast<uint32_t>(
          uint64_t(now)*1000ULL-actual->observed_us) : 1000) : 0);
      w32(r + 38, available ? sample_sequence : 0);
      r[42] = len;
      cache_lengths_[i] = size;
      std::copy(r, r + size, p + pos);
      pos += size;
      ++p[0];
    }
    if (int32_t(due_[i] - next_) < 0)
      next_ = due_[i];
  }
  if (!p[0])
    return false;
  out = encode(0x20, boot_, id_, ++frame_, p, pos);
  return true;
}
bool Session::canPacketCurrent(uint32_t now,const Packet &packet,
                               const CanSamples &can) const {
  if(!active(now) || packet.size<Header+2)return false;
  const uint8_t type=packet.bytes[4];
  if(type!=0x20 && type!=0x21)return false;
  const uint8_t *p=packet.payload();
  const size_t payload=packet.size-Header;
  size_t off=type==0x21?20:2;
  if(type==0x21 && (payload<20 || p[13]!=2 || u16(p+14)!=0 ||
                   u32(p+16)!=snapshot_request_id_))return false;
  if(payload<off)return false;
  const unsigned count=type==0x21?p[12]:p[0];
  if(count>MaxSubscriptions)return false;
  const uint64_t now_us=uint64_t(now)*1000ULL;
  for(unsigned i=0;i<count;++i){
    if(off>payload||payload-off<44)return false;
    const uint8_t *rec=p+off;
    const size_t len=u16(rec);
    if(len<44||len>payload-off||rec[2]!=2)return false;
    if(rec[7]<=1){
      const uint16_t pid=u16(rec+8);
      const auto sample=can.sample(pid);
      uint16_t deadline=0;
      for(unsigned j=0;j<count_;++j){
        const uint8_t *entry=entries_.data()+24*j;
        if(u16(entry)==u16(rec+4)&&u16(entry+4)==pid){
          deadline=u16(entry+20);
          break;
        }
      }
      if(!deadline||!sample||sample->quality>1||!sample->observed_us||
         sample->length!=rec[42]||len!=size_t(44)+rec[42]||
         can.source_generation!=u32(rec+14)||can.mode_epoch!=u32(rec+18)||
         can.source_id!=u32(rec+22)||
         sample->sequence!=u32(rec+38)||
         sample->observed_us!=
             (uint64_t(u32(rec+30))<<32|u32(rec+26))||
         now_us<sample->observed_us||
         now_us>sample->observed_us+uint64_t(deadline)*1000ULL)return false;
      for(unsigned b=0;b<sample->length;++b)
        if(rec[44+b]!=sample->raw[b])return false;
    }
    off+=len;
  }
  return off==payload;
}
} // namespace signal_v2
