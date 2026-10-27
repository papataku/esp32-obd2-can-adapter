#include "signal_v2_session.h"
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
  uint8_t reply[256]{};
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
      size_t snapshot_bytes = 16;
      bool valid = true, relax = false;
      for (unsigned i = 0; i < b[4]; ++i) {
        const auto e = b + 8 + 24 * i;
        uint16_t pid = u16(e + 4);
        snapshot_bytes += 44 + (pid == 0x9a ? 8 : (pid == 0x0c ? 4 : 3));
        valid &= u16(e) != 0 && e[2] == 1 && e[3] == 0 &&
                 (pid == 0x0c || pid == 0x0d || pid == 0x05 || pid == 0x5b ||
                  pid == 0x9a) &&
                 e[6] <= 3 && e[7] == 0 && (pid != 0x0d || e[6] == 0) &&
                 u32(e + 12) == 1 && u16(e + 16) >= 1 && u16(e + 16) <= 10000 &&
                 u16(e + 18) >= u16(e + 16) && u16(e + 20) >= u16(e + 16) &&
                 e[22] == 0 && e[23] == 0;
        if (u16(e + 16) < 200) {
          relax = true;
          valid &= u16(e + 18) >= 200 && u16(e + 20) >= 200;
        }
        for (unsigned j = 0; j < i; ++j)
          if (u16(e) == u16(b + 8 + 24 * j))
            valid = false;
      }
      if (snapshot_bytes > MaxPdu - Header)
        valid = false;
      if (!valid) {
        result = Reject;
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
      else
        snapshot_ = true;
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
  cache_count_ = 0;
  cache_size_ = 0;
  cache_lengths_.fill(0);
  due_.fill(now);
  plan_size_ = 0;
}
bool Session::produce(uint32_t now, Packet &out, const Simulator *source) {
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
    w32(p, frame_ + 2);
    w32(p + 4, 3);
    w32(p + 8, revision_);
    size_t pos = 16;
    for (unsigned i = 0; i < count_; ++i)
      if (cache_lengths_[i]) {
        ++p[12];
        std::copy(cache_.data() + 52 * i,
                  cache_.data() + 52 * i + cache_lengths_[i], p + pos);
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
      r[2] = 1;
      r[3] = 2;
      w16(r + 4, u16(e));
      r[6] = 1;
      r[7] = pid == 0x0c ? 1 : 0;
      w16(r + 8, pid);
      w32(r + 10, revision_);
      w32(r + 14, 1);
      w32(r + 18, 1);
      w32(r + 22, u32(e + 8));
      auto shared = source ? source->sample(pid) : nullptr;
      w64(r + 26, uint64_t(shared ? shared->observed_ms : now) * 1000);
      w32(r + 34, 1000);
      w32(r + 38, shared ? shared->sequence : ++sample_);
      r[42] = len;
      r[44] = 0x41;
      r[45] = pid;
      if (pid == 0x0d)
        r[46] = 42;
      else if (pid == 5)
        r[46] = 120;
      else if (pid == 0x5b)
        r[46] = 128;
      else if (pid == 0x9a) {
        r[48] = 0x64;
        r[51] = 100;
      }
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
} // namespace signal_v2
