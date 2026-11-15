#pragma once
// Signal V2 flight recorder (PM Claude 2026-11-15). Read-only telemetry.
//
// Producer: BLE application owner task (signal_v2::poll), after CanOwner::tick.
//   It only READS owner state through const references / copied values and
//   pushes small records into a bounded lock-free SPSC byte ring. It never
//   blocks, never touches flash, never submits CAN traffic. A full ring drops
//   the record and counts the drop.
// Consumer: Arduino/UI loop. Drains the ring into bounded RAM pages and
//   persists them to the owner-protected EvidenceStore ("spiffs") only
//   1) once at boot, before TWAI starts, and
//   2) after a LATCHED owner stop, once no transaction is in flight, under the
//      existing CanMonitor CAN-idle log checkpoint.
//   Any store failure disables persistence for the rest of the boot.
// No BLE address, vehicle identifier, location or key material is recorded. Links are
// identified only by per-boot slot index and per-boot generation counters.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "signal_v2_can_samples.h"
#include "signal_v2_flight_recorder_tap.h"
#include "signal_v2_pid_diagnostics.h"

namespace signal_v2 {
namespace recorder {

constexpr uint32_t kPageMagic = 0x50523256u;  // "V2RP" little endian
constexpr uint16_t kFormatVersion = 1;
constexpr unsigned kSlots = 48;               // EvidenceStore::V2Rec keys
constexpr size_t kPageBytes = 2304;           // == EvidenceStore::kMaxBlob
constexpr size_t kPageHeader = 28;
constexpr size_t kPayload = kPageBytes - kPageHeader;
constexpr unsigned kRamPages = 5;             // 1 pinned head + 4 tail pages
constexpr unsigned kMaxPutsPerBoot = 12;      // boot page + flush pages
constexpr size_t kRingBytes = 2048;
constexpr uint32_t kSummaryMs = 10000;
constexpr uint32_t kLatchedSummaryMs = 60000;
constexpr uint32_t kLiveMs = 1000;
constexpr uint32_t kReflushMs = 60000;
constexpr uint8_t kAbnormalStreakLimit = 8;

enum RecordType : uint8_t {
  RecBoot = 1, RecEvent = 2, RecStop = 3, RecSummary = 4, RecLive = 5, RecSummary2 = 6,
  RecBle = 7, RecText = 8
};
enum EventCode : uint8_t {
  EvArm = 1, EvDisarm = 2, EvManualStop = 3, EvEcuPin = 4, EvEcuUnpin = 5,
  EvLinkUp = 6, EvLinkDown = 7, EvLinkSub = 8, EvLinkUnsub = 9, EvStopIdle = 10,
  EvRingDrop = 12, EvFlush = 13, EvStoreFail = 14, EvBootSuppressed = 15,
  EvAuthEnd = 16, EvEcuState = 17
};
enum PageKind : uint16_t { BootPage = 1, FlushPage = 2 };
enum PageFlags : uint16_t { PageOverflow = 1, PageHead = 2, PageRewrite = 4 };

inline uint16_t sat16(uint64_t v) { return v > 0xFFFFu ? 0xFFFFu : uint16_t(v); }
inline uint32_t sat32(uint64_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(v); }
inline uint64_t delta(uint64_t now, uint64_t base) { return now >= base ? now - base : 0; }
inline void inc(uint32_t &v) { if (v != UINT32_MAX) ++v; }

struct Writer {
  uint8_t *p;
  size_t cap, n = 0;
  bool ok = true;
  Writer(uint8_t *b, size_t c) : p(b), cap(c) {}
  void u8(uint32_t v) { if (n + 1 > cap) { ok = false; return; } p[n++] = uint8_t(v); }
  void u16(uint32_t v) { u8(v); u8(v >> 8); }
  void u32(uint32_t v) { u16(v); u16(v >> 16); }
  void bytes(const void *src, size_t len) {
    if (n + len > cap) { ok = false; return; }
    std::memcpy(p + n, src, len); n += len;
  }
};
struct Reader {
  const uint8_t *p;
  size_t cap, n = 0;
  bool ok = true;
  Reader(const uint8_t *b, size_t c) : p(b), cap(c) {}
  uint32_t u8() { if (n + 1 > cap) { ok = false; return 0; } return p[n++]; }
  uint32_t u16() { uint32_t a = u8(); return a | (u8() << 8); }
  uint32_t u32() { uint32_t a = u16(); return a | (u16() << 16); }
};

// Single producer (BLE app task) / single consumer (Arduino loop) byte ring.
// Records are [type u8][len u8][payload len]. Never blocks.
class SpscRing {
 public:
  static_assert((kRingBytes & (kRingBytes - 1)) == 0, "power of two");
  bool push(const uint8_t *rec, size_t len) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    if (len > kRingBytes - (h - t)) {
      const uint32_t d = drops_.load(std::memory_order_relaxed);
      if (d != UINT32_MAX) drops_.store(d + 1, std::memory_order_relaxed);
      return false;
    }
    for (size_t i = 0; i < len; ++i) buf_[(h + i) & (kRingBytes - 1)] = rec[i];
    head_.store(h + uint32_t(len), std::memory_order_release);
    return true;
  }
  size_t pop(uint8_t *out, size_t cap) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    if (h == t) return 0;
    const size_t len = size_t(buf_[(t + 1) & (kRingBytes - 1)]) + 2;
    if (len > cap || len > h - t) return 0;  // impossible with one producer
    for (size_t i = 0; i < len; ++i) out[i] = buf_[(t + i) & (kRingBytes - 1)];
    tail_.store(t + uint32_t(len), std::memory_order_release);
    return len;
  }
  uint32_t drops() const { return drops_.load(std::memory_order_relaxed); }

 private:
  uint8_t buf_[kRingBytes]{};
  std::atomic<uint32_t> head_{0}, tail_{0}, drops_{0};
};

// Values copied from owner/GATT state by the producer hook. No identities.
struct OwnerView {
  bool armed = false, manual = false, stopped = false, busy = false;
  const char *reason = nullptr;  // latched owner stop reason (string literal)
  uint32_t epoch = 0;
  bool link[2]{}, subscribed[2]{};
  uint32_t generation[2]{};
};
// Plain subset of m5can::CanStats (filled by the device glue under its lock).
struct CanCounters {
  uint64_t tx_attempts = 0, tx_success = 0, query_count = 0, query_ok = 0,
           query_timeout = 0, query_no_data = 0, query_failed = 0,
           v2_rx = 0, v2_fc = 0, app_queue_drops = 0;
  uint32_t nrc = 0, status[10]{}, rx_missed = 0, rx_overrun = 0, bus_error = 0,
           rx_queue_depth = 0;
  uint8_t state = 0;
  bool fault_locked = false;
};
using CanReader = void (*)(CanCounters &);
using ErrorReader = void (*)(char *out, size_t cap);  // CanMonitor::lastError copy
// Where an already queued DATA/SNAPSHOT/other PDU was discarded by GATT.
enum DiscardSite : uint8_t {
  DiscardSourceTransition = 0,  // acquisition/invalidate/STOP clears queues
  DiscardStaleInSend = 1,       // send(): STALE_OR_INVALID queue_drop
  DiscardControlAccept = 2,     // accepted control op clears pending output
  DiscardLeaseExpired = 3       // lease expiry before notify
};
enum AuthEndCode : uint8_t {
  AuthSessionEnded = 1, AuthUsbArmExpired = 2, AuthOperatorStop = 3,
  AuthControlFifoOverflow = 4, AuthCentralUnsubscribe = 5, AuthAutoArmGranted = 6
};
enum TextKind : uint8_t { TextCanError = 1 };

class Producer {
 public:
  void attach(SpscRing *ring, CanReader reader, ErrorReader error = nullptr) {
    ring_ = ring; reader_ = reader; error_ = error;
  }
  // Ring control results (op 1..11 of the Signal V2 control protocol).
  void control(uint8_t op, uint8_t result) {
    const bool ok = result <= 1;  // Accept / Relaxed
    if (op == 4) inc(ok ? cum_.op4_ok : cum_.op4_rej);
    else if (op == 7) inc(ok ? cum_.op7_ok : cum_.op7_rej);
    else inc(ok ? cum_.other_ok : cum_.other_rej);
    if (!ok) { last_rej_op_ = op; last_rej_code_ = result; }
  }
  void produced(uint8_t type, uint8_t records) {
    if (type == 0x20) inc(cum_.data_pkts);
    else if (type == 0x21) {
      inc(cum_.snapshot_pkts);
      if (!records) inc(cum_.snapshot_empty);
      for (unsigned k = 0; k < records; ++k) inc(cum_.snapshot_records);
    } else inc(cum_.other_pkts);
  }
  void discarded(uint8_t site, uint8_t type) {
    if (site > 3) return;
    inc(cum_.discard[site][type == 0x20 ? 0 : type == 0x21 ? 1 : 2]);
  }
  void authEnd(uint32_t now, uint8_t code) { if (ring_) event(now, EvAuthEnd, code, 0, 0); }
  void observe(uint32_t now, const OwnerView &v, const PidDiagnostics &d,
               const CanSamples &s, const ResultTap &tap) {
    if (!ring_) return;
    if (!started_) {
      started_ = true;
      prev_ = v; prev_.reason = nullptr; prev_.armed = false; prev_.manual = false;
      for (unsigned i = 0; i < 2; ++i) { prev_.link[i] = prev_.subscribed[i] = false; }
      CanCounters c{};
      if (reader_) reader_(c);
      baseline(now, d, s, c, tap);
      prev_inv_ = s.invalidations; prev_source_ = 0;
      for (unsigned i = 0; i < 5; ++i) prev_seq_[i] = s.values[i].sequence;
      last_live_ = last_error_check_ = now;
    }
    for (unsigned i = 0; i < 2; ++i) {
      if (v.link[i] != prev_.link[i])
        event(now, v.link[i] ? EvLinkUp : EvLinkDown, uint8_t(i), 0, v.generation[i]);
      const bool sub = v.link[i] && v.subscribed[i];
      const bool was = prev_.link[i] && prev_.subscribed[i];
      if (sub != was) event(now, sub ? EvLinkSub : EvLinkUnsub, uint8_t(i), 0, v.generation[i]);
    }
    if (v.armed != prev_.armed)
      event(now, v.armed ? EvArm : EvDisarm,
            uint8_t((v.manual ? 1 : 0) | (v.stopped ? 2 : 0) | (v.busy ? 4 : 0)), 0, v.epoch);
    if (v.manual && !prev_.manual) event(now, EvManualStop, 0, 0, v.epoch);
    if (s.source_id != prev_source_) {
      if (prev_source_) event(now, EvEcuUnpin, 0, 0, prev_source_);
      if (s.source_id) event(now, EvEcuPin, 0, 0, s.source_id);
      prev_source_ = s.source_id;
    }
    if (tap.ecu_state != prev_ecu_state_) {
      event(now, EvEcuState, tap.ecu_state, tap.ecu_bad, prev_ecu_state_);
      prev_ecu_state_ = tap.ecu_state;
    }
    if (v.epoch != epoch_) {
      epoch_ = v.epoch;
      for (auto &o : last_obs_us_) o = 0;
    }
    for (unsigned i = 0; i < 5; ++i) {
      const auto &x = s.values[i];
      if (x.sequence == prev_seq_[i]) continue;
      prev_seq_[i] = x.sequence;
      if (x.quality > 1 || !x.observed_us) continue;
      inc(cum_.samples[i]);
      if (last_obs_us_[i] && x.observed_us > last_obs_us_[i]) {
        const uint32_t gap = sat32((x.observed_us - last_obs_us_[i]) / 1000);
        if (gap > win_gap_[i]) win_gap_[i] = gap;
        if (gap > cum_.max_gap[i]) cum_.max_gap[i] = gap;
      }
      last_obs_us_[i] = x.observed_us;
    }
    if (s.invalidations != prev_inv_) {
      const uint32_t n = s.invalidations - prev_inv_;
      prev_inv_ = s.invalidations;
      const uint8_t q = s.values[0].quality;
      uint32_t &cls = q == 3 ? cum_.inv_nodata : q == 5 ? cum_.inv_verify
                    : (q == 6 || q == 10) ? cum_.inv_stop : cum_.inv_other;
      inc(cls);
      for (uint32_t k = 1; k < n && k < 1000; ++k) inc(cum_.inv_other);
      last_quality_ = q;
    }
    if (tap.nodata_run > win_nodata_run_) win_nodata_run_ = tap.nodata_run;
    if (error_ && (uint32_t(now - last_error_check_) >= 250 || (v.reason && !prev_.reason))) {
      last_error_check_ = now;
      char e[48]{};
      error_(e, sizeof(e));
      if (std::strncmp(e, last_error_, sizeof(e))) {
        std::memcpy(last_error_, e, sizeof(e));
        text(now, TextCanError, e);
      }
    }
    if (v.reason && !prev_.reason) {
      summary(now, v, d, s, tap);
      stopRecord(now, v, s, tap);
      live(now, v, d, s, tap);
      latched_ = true;
    }
    if (latched_ && !stop_idle_ && v.stopped && !v.busy) {
      stop_idle_ = true;
      event(now, EvStopIdle, 0, 0, v.epoch);
    }
    const bool active = v.armed || v.link[0] || v.link[1];
    const uint32_t period = latched_ ? kLatchedSummaryMs : kSummaryMs;
    if (uint32_t(now - window_start_) >= period && active &&
        (!latched_ || changed(d, s, tap)))
      summary(now, v, d, s, tap);
    if (uint32_t(now - last_live_) >= kLiveMs) live(now, v, d, s, tap);
    prev_ = v;
  }
  bool latched() const { return latched_; }

  // Cumulative ring-service and quality counters (owner task only).
  struct Cum {
    uint32_t samples[5]{}, max_gap[5]{};
    uint32_t inv_nodata = 0, inv_verify = 0, inv_stop = 0, inv_other = 0;
    uint32_t data_pkts = 0, snapshot_pkts = 0, other_pkts = 0, snapshot_empty = 0,
             snapshot_records = 0;
    uint32_t op4_ok = 0, op4_rej = 0, op7_ok = 0, op7_rej = 0, other_ok = 0, other_rej = 0;
    uint32_t discard[4][3]{};
  };
  Cum &testCum() { return cum_; }  // host fault injection only

 private:
  void push(Writer &w) {
    if (!w.ok) return;
    w.p[1] = uint8_t(w.n - 2);
    ring_->push(w.p, w.n);
  }
  void event(uint32_t now, uint8_t code, uint8_t a8, uint16_t a16, uint32_t a32) {
    uint8_t b[14];
    Writer w(b, sizeof(b));
    w.u8(RecEvent); w.u8(0); w.u32(now); w.u8(code); w.u8(a8); w.u16(a16); w.u32(a32);
    push(w);
  }
  void text(uint32_t now, uint8_t kind, const char *s) {
    uint8_t b[60];
    Writer w(b, sizeof(b));
    size_t len = 0;
    while (s[len] && len < 47) ++len;
    w.u8(RecText); w.u8(0); w.u32(now); w.u8(kind); w.u8(uint32_t(len)); w.bytes(s, len);
    push(w);
  }
  void baseline(uint32_t now, const PidDiagnostics &d, const CanSamples &s,
                const CanCounters &c, const ResultTap &tap) {
    window_start_ = now;
    for (unsigned i = 0; i < 5; ++i) {
      const auto &p = d.pid[i];
      base_pid_[i] = {{p.demand, p.admitted, p.completed, p.failed, p.shed, p.denied, p.cancelled}};
      win_gap_[i] = 0;
    }
    base_cum_ = cum_;
    base_acquired_ = s.acquired;
    base_inv_ = s.invalidations;
    base_can_ = c;
    base_tap_ = tap;
    win_nodata_run_ = tap.nodata_run;
  }
  bool changed(const PidDiagnostics &d, const CanSamples &s, const ResultTap &tap) const {
    if (s.acquired != base_acquired_ || s.invalidations != base_inv_ ||
        tap.results != base_tap_.results) return true;
    for (unsigned i = 0; i < 5; ++i) {
      const auto &p = d.pid[i];
      const uint64_t cur[7] = {p.demand, p.admitted, p.completed, p.failed, p.shed, p.denied, p.cancelled};
      for (unsigned k = 0; k < 7; ++k) if (cur[k] != base_pid_[i][k]) return true;
    }
    return std::memcmp(&cum_, &base_cum_, sizeof(cum_)) != 0;
  }
  uint8_t flags(const OwnerView &v, const CanSamples &s) const {
    return uint8_t((v.armed ? 1 : 0) | (v.stopped ? 2 : 0) | (v.busy ? 4 : 0) |
                   (v.manual ? 8 : 0) | (s.source_id ? 16 : 0) | (v.link[0] ? 32 : 0) |
                   (v.link[1] ? 64 : 0) | ((v.subscribed[0] || v.subscribed[1]) ? 128 : 0));
  }
  // Two records per window: owner/PID view, then CAN/GATT/result-tap view.
  void summary(uint32_t now, const OwnerView &v, const PidDiagnostics &d, const CanSamples &s,
               const ResultTap &tap) {
    CanCounters c{};
    if (reader_) reader_(c);
    uint8_t b[256];
    {
      Writer w(b, sizeof(b));
      w.u8(RecSummary); w.u8(0);
      w.u32(now); w.u32(now - window_start_); w.u8(flags(v, s)); w.u8(last_quality_); w.u32(v.epoch);
      for (unsigned i = 0; i < 5; ++i) {
        const auto &p = d.pid[i];
        const uint64_t cur[7] = {p.demand, p.admitted, p.completed, p.failed, p.shed, p.denied, p.cancelled};
        for (unsigned k = 0; k < 7; ++k) w.u16(sat16(delta(cur[k], base_pid_[i][k])));
        w.u16(sat16(delta(cum_.samples[i], base_cum_.samples[i])));
        w.u16(sat16(win_gap_[i]));
      }
      w.u16(sat16(delta(s.acquired, base_acquired_)));
      w.u16(sat16(delta(s.invalidations, base_inv_)));
      w.u16(sat16(delta(cum_.inv_nodata, base_cum_.inv_nodata)));
      w.u16(sat16(delta(cum_.inv_verify, base_cum_.inv_verify)));
      w.u16(sat16(delta(cum_.inv_stop, base_cum_.inv_stop)));
      w.u16(sat16(delta(cum_.inv_other, base_cum_.inv_other)));
      w.u8(tap.ecu_state); w.u8(tap.ecu_bad);
      w.u16(sat16(win_nodata_run_)); w.u16(sat16(tap.nodata_run_max));
      push(w);
    }
    {
      Writer w(b, sizeof(b));
      w.u8(RecSummary2); w.u8(0);
      w.u32(now);
      const uint64_t can_now[10] = {c.tx_attempts, c.tx_success, c.query_count, c.query_ok,
          c.query_timeout, c.query_no_data, c.query_failed, c.v2_rx, c.v2_fc, c.app_queue_drops};
      const uint64_t can_base[10] = {base_can_.tx_attempts, base_can_.tx_success,
          base_can_.query_count, base_can_.query_ok, base_can_.query_timeout,
          base_can_.query_no_data, base_can_.query_failed, base_can_.v2_rx, base_can_.v2_fc,
          base_can_.app_queue_drops};
      for (unsigned k = 0; k < 10; ++k) w.u16(sat16(delta(can_now[k], can_base[k])));
      w.u16(sat16(delta(c.nrc, base_can_.nrc)));
      for (unsigned k = 0; k < 10; ++k) w.u16(sat16(delta(c.status[k], base_can_.status[k])));
      w.u16(sat16(c.rx_missed)); w.u16(sat16(c.rx_overrun)); w.u16(sat16(c.bus_error));
      w.u8(c.state); w.u8(c.rx_queue_depth > 255 ? 255 : c.rx_queue_depth);
      w.u8(c.fault_locked ? 1 : 0);
      const uint32_t g_now[11] = {cum_.data_pkts, cum_.snapshot_pkts, cum_.other_pkts,
          cum_.snapshot_empty, cum_.snapshot_records, cum_.op4_ok, cum_.op4_rej, cum_.op7_ok,
          cum_.op7_rej, cum_.other_ok, cum_.other_rej};
      const uint32_t g_base[11] = {base_cum_.data_pkts, base_cum_.snapshot_pkts,
          base_cum_.other_pkts, base_cum_.snapshot_empty, base_cum_.snapshot_records,
          base_cum_.op4_ok, base_cum_.op4_rej, base_cum_.op7_ok, base_cum_.op7_rej,
          base_cum_.other_ok, base_cum_.other_rej};
      for (unsigned k = 0; k < 11; ++k) w.u16(sat16(delta(g_now[k], g_base[k])));
      w.u8(last_rej_op_); w.u8(last_rej_code_);
      for (unsigned site = 0; site < 4; ++site)
        for (unsigned t = 0; t < 3; ++t)
          w.u16(sat16(delta(cum_.discard[site][t], base_cum_.discard[site][t])));
      w.u16(sat16(delta(tap.results, base_tap_.results)));
      for (unsigned k = 0; k < ResultTap::kBuckets; ++k)
        w.u16(sat16(delta(tap.latency[k], base_tap_.latency[k])));
      w.u16(sat16(delta(tap.multi_frame, base_tap_.multi_frame)));
      w.u8(tap.last_status); w.u8(tap.last_responses); w.u8(tap.last_raw);
      w.u8(tap.last_sf); w.u8(tap.last_ff); w.u8(tap.last_cf);
      w.u32(tap.last_first_us); w.u32(tap.last_ff_us); w.u32(tap.last_cf_us);
      push(w);
    }
    baseline(now, d, s, c, tap);
  }
  void stopRecord(uint32_t now, const OwnerView &v, const CanSamples &s, const ResultTap &tap) {
    uint8_t b[150];
    Writer w(b, sizeof(b));
    w.u8(RecStop); w.u8(0);
    w.u32(now); w.u32(v.epoch); w.u8(flags(v, s)); w.u8(last_quality_);
    w.u32(s.source_id); w.u32(s.acquired); w.u32(s.invalidations);
    const uint64_t now_us = uint64_t(now) * 1000;
    for (unsigned i = 0; i < 5; ++i) {
      const uint64_t last = last_obs_us_[i];
      w.u16(last && now_us >= last ? sat16((now_us - last) / 1000) : 0xFFFF);
    }
    w.u8(tap.last_status); w.u8(tap.last_responses); w.u8(tap.last_raw);
    w.u8(tap.ecu_state); w.u8(tap.ecu_bad);
    w.u16(sat16(tap.nodata_run)); w.u16(sat16(tap.nodata_run_max));
    size_t len = 0;
    while (v.reason && v.reason[len] && len < 48) ++len;
    w.u8(uint32_t(len));
    w.bytes(v.reason, len);
    len = 0;
    while (last_error_[len] && len < 47) ++len;
    w.u8(uint32_t(len));
    w.bytes(last_error_, len);
    push(w);
  }
  void live(uint32_t now, const OwnerView &v, const PidDiagnostics &d, const CanSamples &s,
            const ResultTap &tap) {
    last_live_ = now;
    uint8_t b[256];
    Writer w(b, sizeof(b));
    w.u8(RecLive); w.u8(0);
    w.u32(now); w.u32(v.epoch); w.u8(flags(v, s)); w.u8(last_quality_);
    w.u32(s.acquired); w.u32(s.invalidations); w.u32(s.source_id);
    w.u32(cum_.inv_nodata); w.u32(cum_.inv_verify); w.u32(cum_.inv_stop); w.u32(cum_.inv_other);
    for (unsigned i = 0; i < 5; ++i) {
      const auto &p = d.pid[i];
      const uint64_t cur[7] = {p.demand, p.admitted, p.completed, p.failed, p.shed, p.denied, p.cancelled};
      for (unsigned k = 0; k < 7; ++k) w.u32(sat32(cur[k]));
      w.u32(cum_.samples[i]);
      w.u16(sat16(cum_.max_gap[i]));
    }
    w.u32(cum_.data_pkts); w.u32(cum_.snapshot_pkts);
    w.u16(sat16(cum_.op4_ok)); w.u16(sat16(cum_.op4_rej));
    w.u16(sat16(cum_.op7_ok)); w.u16(sat16(cum_.op7_rej));
    w.u32(ring_->drops());
    w.u16(sat16(tap.status[1])); w.u16(sat16(tap.nodata_run_max));
    push(w);
  }

  SpscRing *ring_ = nullptr;
  CanReader reader_ = nullptr;
  ErrorReader error_ = nullptr;
  bool started_ = false, latched_ = false, stop_idle_ = false;
  OwnerView prev_{};
  uint32_t prev_inv_ = 0, prev_source_ = 0, epoch_ = 0;
  uint32_t prev_seq_[5]{};
  uint64_t last_obs_us_[5]{};
  uint32_t win_gap_[5]{};
  uint32_t window_start_ = 0, last_live_ = 0, last_error_check_ = 0, win_nodata_run_ = 0;
  uint8_t last_quality_ = 2, last_rej_op_ = 0, last_rej_code_ = 0, prev_ecu_state_ = 0;
  char last_error_[48]{};
  Cum cum_{}, base_cum_{};
  std::array<std::array<uint64_t, 7>, 5> base_pid_{};
  uint32_t base_acquired_ = 0, base_inv_ = 0;
  CanCounters base_can_{};
  ResultTap base_tap_{};
};

// Persistent page slots (implemented by the EvidenceStore adapter on device).
class PageStore {
 public:
  virtual ~PageStore() = default;
  virtual bool ready() const = 0;
  virtual bool put(unsigned slot, const uint8_t *data, size_t bytes) = 0;
  virtual size_t length(unsigned slot) const = 0;
  virtual size_t get(unsigned slot, uint8_t *data, size_t bytes) = 0;
};
// The CAN-idle reservation shared with every other flash writer.
class Checkpoint {
 public:
  virtual ~Checkpoint() = default;
  virtual bool begin() = 0;
  virtual void end() = 0;
};

struct PageHeader {
  uint32_t magic = 0;
  uint16_t version = 0, kind = 0;
  uint32_t page_seq = 0, boot_id = 0;
  uint16_t page_in_boot = 0, records = 0, payload = 0, flags = 0;
  uint32_t dropped = 0;
};
inline bool parseHeader(const uint8_t *b, size_t n, PageHeader &h) {
  if (n < kPageHeader) return false;
  Reader r(b, n);
  h.magic = r.u32(); h.version = uint16_t(r.u16()); h.kind = uint16_t(r.u16());
  h.page_seq = r.u32(); h.boot_id = r.u32(); h.page_in_boot = uint16_t(r.u16());
  h.records = uint16_t(r.u16()); h.payload = uint16_t(r.u16()); h.flags = uint16_t(r.u16());
  h.dropped = r.u32();
  return r.ok && h.magic == kPageMagic && h.version == kFormatVersion &&
         (h.kind == BootPage || h.kind == FlushPage) && h.payload <= n - kPageHeader &&
         h.payload <= kPayload;
}
inline bool abnormalReset(uint32_t reason) {
  // esp_reset_reason(): PANIC, INT_WDT, TASK_WDT, WDT, BROWNOUT.
  return reason == 4 || reason == 5 || reason == 6 || reason == 7 || reason == 9;
}

struct BootInfo {
  const char *sha = "unknown";
  uint32_t reset = 0, rom_reset = 0;
};

class Consumer {
 public:
  struct Status {
    bool store_ready = false, store_failed = false, boot_written = false,
         boot_suppressed = false, window_open = false;
    uint32_t boot_id = 0, next_page_seq = 0, retained = 0, puts = 0, put_failures = 0,
             dropped_evicted = 0, dropped_post = 0, ring_drops = 0, records = 0;
    uint8_t streak = 0;
  };
  struct Page {
    uint8_t bytes[kPayload];
    uint16_t used = 0, flushed = 0, records = 0, page_in_boot = 0, flags = 0;
    uint32_t dropped_before = 0;
    bool in_use = false;
    void reset() {
      used = flushed = records = page_in_boot = flags = 0;
      dropped_before = 0;
      in_use = false;
    }
  };

  void boot(PageStore *store, const BootInfo &info, uint32_t now) {
    store_ = store;
    status_ = Status{};
    for (auto &p : pages_) p.reset();
    head_sealed_ = false; tail_count_ = 0; tail_first_ = 1; next_page_in_boot_ = 1;
    pending_overflow_ = open_pending_ = round_ = false; rounds_ = 0; last_round_ = 0; live_len_ = 0;
    pages_[0].in_use = true; pages_[0].page_in_boot = next_page_in_boot_++;
    pages_[0].flags = PageHead;
    uint32_t max_seq = 0, max_boot = 0;
    bool any = false, last_was_boot = false;
    uint8_t last_streak = 0;
    status_.store_ready = store_ && store_->ready();
    if (status_.store_ready) {
      for (unsigned slot = 0; slot < kSlots; ++slot) {
        const size_t n = store_->length(slot);
        if (!n || n > kPageBytes || store_->get(slot, scratch_, n) != n) continue;
        PageHeader h{};
        if (!parseHeader(scratch_, n, h)) continue;
        ++status_.retained;
        if (h.boot_id > max_boot) max_boot = h.boot_id;
        if (!any || h.page_seq > max_seq) {
          any = true; max_seq = h.page_seq;
          last_was_boot = h.kind == BootPage;
          last_streak = last_was_boot ? bootStreak(scratch_, n) : 0;
        }
      }
    }
    status_.boot_id = max_boot + 1;
    status_.next_page_seq = any ? max_seq + 1 : 1;
    const bool abnormal = abnormalReset(info.reset);
    status_.streak = !abnormal ? 0 : last_was_boot ? uint8_t(last_streak < 255 ? last_streak + 1 : 255) : 1;
    // Bounded boot-loop wear: an abnormal-reset streak stops writing boot pages.
    status_.boot_suppressed = abnormal && last_was_boot && last_streak >= kAbnormalStreakLimit;
    uint8_t b[80];
    Writer w(b, sizeof(b));
    w.u8(RecBoot); w.u8(0);
    w.u32(status_.boot_id); w.u32(status_.next_page_seq);
    w.u8(info.reset); w.u8(info.rom_reset); w.u8(status_.streak);
    w.u8((status_.store_ready ? 1 : 0) | (status_.boot_suppressed ? 2 : 0));
    w.u16(status_.retained);
    char sha[41]{};
    std::strncpy(sha, info.sha ? info.sha : "unknown", 40);
    w.bytes(sha, 40);
    b[1] = uint8_t(w.n - 2);
    append(b, w.n);
    if (status_.store_ready && !status_.boot_suppressed) {
      const size_t n = encodePage(scratch_, BootPage, 0, pages_[0].bytes, pages_[0].used,
                                  pages_[0].records, PageHead, 0);
      status_.boot_written = writePage(scratch_, n);
    } else if (status_.boot_suppressed) {
      event(now, EvBootSuppressed, status_.streak, 0, 0);
    }
  }
  // Consumer-side record (BLE GAP diagnostics, ring drop notes).
  void append(const uint8_t *rec, size_t len) {
    if (len < 2 || len > 257) return;
    inc(status_.records);
    Page &head = pages_[0];
    if (!head_sealed_) {
      if (head.used + len <= kPayload) { put(head, rec, len); return; }
      head_sealed_ = true;
    }
    Page *cur = tail_count_ ? &pages_[index(tail_count_ - 1)] : nullptr;
    if (cur && cur->used + len <= kPayload) { put(*cur, rec, len); return; }
    if (tail_count_ < kRamPages - 1) {
      ++tail_count_;
    } else {
      Page &oldest = pages_[index(0)];
      // Once a latched stop opened the window, never evict pre-stop evidence
      // that was not yet persisted: drop the newest record instead.
      if (status_.window_open && oldest.used > oldest.flushed) { inc(status_.dropped_post); return; }
      if (oldest.used > oldest.flushed) {
        for (uint32_t k = 0; k < oldest.records; ++k) inc(status_.dropped_evicted);
        pending_overflow_ = true;
      }
      tail_first_ = tail_first_ % (kRamPages - 1) + 1;
    }
    Page &fresh = pages_[index(tail_count_ - 1)];
    fresh.reset();
    fresh.in_use = true;
    fresh.page_in_boot = next_page_in_boot_ < 0xFFFF ? next_page_in_boot_++ : 0xFFFF;
    fresh.dropped_before = status_.dropped_evicted;
    if (pending_overflow_) { fresh.flags |= PageOverflow; pending_overflow_ = false; }
    put(fresh, rec, len);
  }
  // Drain producer records. Bounded per call.
  void drain(SpscRing &ring, uint32_t now, unsigned max_records = 32) {
    uint8_t rec[260];
    for (unsigned i = 0; i < max_records; ++i) {
      const size_t n = ring.pop(rec, sizeof(rec));
      if (!n) break;
      if (rec[0] == RecLive) { std::memcpy(live_, rec, n); live_len_ = n; continue; }
      if (rec[0] == RecEvent && n >= 7 && rec[6] == EvStopIdle) open_pending_ = true;
      append(rec, n);
    }
    const uint32_t drops = ring.drops();
    if (drops != status_.ring_drops) {
      event(now, EvRingDrop, 0, 0, drops);
      status_.ring_drops = drops;
    }
  }
  // At most ONE store write per call, only inside the latched-stop window.
  bool flushStep(uint32_t now, Checkpoint &cp) {
    if (open_pending_ && !status_.window_open) {
      status_.window_open = true;
      if (live_len_) append(live_, live_len_);  // cumulative totals at stop
      round_ = true;
    }
    if (!status_.window_open || !status_.store_ready || status_.store_failed) return false;
    if (status_.puts >= kMaxPutsPerBoot) return false;
    Page *dirty = nullptr;
    for (unsigned k = 0; k < kRamPages && !dirty; ++k) {
      Page &p = k == 0 ? pages_[0] : pages_[index(k - 1)];
      if (k && k > tail_count_) break;
      if (p.in_use && p.used > p.flushed) dirty = &p;
    }
    if (!dirty) {
      if (round_) { round_ = false; last_round_ = now; ++rounds_; }
      return false;
    }
    if (!round_) {
      if (uint32_t(now - last_round_) < kReflushMs) return false;
      round_ = true;
    }
    if (!cp.begin()) return false;  // a CAN transaction owns the bus: retry later
    const uint16_t flags = uint16_t(dirty->flags | (dirty->flushed ? PageRewrite : 0));
    const size_t n = encodePage(scratch_, FlushPage, dirty->page_in_boot, dirty->bytes,
                                dirty->used, dirty->records, flags, dirty->dropped_before);
    const bool ok = writePage(scratch_, n);
    cp.end();
    if (ok) dirty->flushed = dirty->used;
    return ok;
  }
  const Status &status() const { return status_; }
  const uint8_t *live(size_t &n) const { n = live_len_; return live_; }
  // RAM page enumeration for DUMP (unpersisted tail included).
  unsigned ramPages() const { return 1 + tail_count_; }
  const Page &ramPage(unsigned k) const { return k == 0 ? pages_[0] : pages_[index(k - 1)]; }

  static size_t encodePage(uint8_t *out, uint16_t kind, uint16_t page_in_boot,
                           const uint8_t *payload, size_t used, uint16_t records,
                           uint16_t flags, uint32_t dropped, uint32_t seq = 0, uint32_t boot = 0) {
    Writer w(out, kPageBytes);
    w.u32(kPageMagic); w.u16(kFormatVersion); w.u16(kind); w.u32(seq); w.u32(boot);
    w.u16(page_in_boot); w.u16(records); w.u16(uint32_t(used)); w.u16(flags); w.u32(dropped);
    w.bytes(payload, used);
    return w.ok ? w.n : 0;
  }

 private:
  static uint8_t bootStreak(const uint8_t *page, size_t n) {
    // First record of a boot page is RecBoot: type,len,boot_id,seq,reset,rom,streak
    if (n < kPageHeader + 13 || page[kPageHeader] != RecBoot) return 0;
    return page[kPageHeader + 2 + 10];
  }
  unsigned index(unsigned k) const {  // k-th oldest tail page -> pages_ index 1..
    return 1 + (tail_first_ - 1 + k) % (kRamPages - 1);
  }
  static void put(Page &p, const uint8_t *rec, size_t len) {
    std::memcpy(p.bytes + p.used, rec, len);
    p.used = uint16_t(p.used + len);
    ++p.records;
  }
  void event(uint32_t now, uint8_t code, uint8_t a8, uint16_t a16, uint32_t a32) {
    uint8_t b[14];
    Writer w(b, sizeof(b));
    w.u8(RecEvent); w.u8(12); w.u32(now); w.u8(code); w.u8(a8); w.u16(a16); w.u32(a32);
    append(b, w.n);
  }
  bool writePage(uint8_t *page, size_t n) {
    if (!n || !store_ || status_.store_failed || status_.puts >= kMaxPutsPerBoot) return false;
    const uint32_t seq = status_.next_page_seq;
    Writer w(page + 8, 8);
    w.u32(seq); w.u32(status_.boot_id);
    inc(status_.puts);
    if (!store_->put(seq % kSlots, page, n)) {
      // Fail closed: never retry or touch flash again during this boot.
      status_.store_failed = true;
      inc(status_.put_failures);
      event(0, EvStoreFail, 0, 0, seq);
      return false;
    }
    status_.next_page_seq = seq + 1;
    return true;
  }

  PageStore *store_ = nullptr;
  Status status_{};
  Page pages_[kRamPages];
  bool head_sealed_ = false, pending_overflow_ = false, open_pending_ = false, round_ = false;
  unsigned tail_count_ = 0, tail_first_ = 1, rounds_ = 0;
  uint16_t next_page_in_boot_ = 1;
  uint32_t last_round_ = 0;
  uint8_t live_[260]{};
  size_t live_len_ = 0;
  uint8_t scratch_[kPageBytes]{};
};

// Text form of one record for USB DUMP/STATUS (no identities exist to print).
// tools/decode_v2_flight_recorder.py implements the same layout.
class Formatter {
 public:
  Formatter(char *out, size_t cap) : out_(out), cap_(cap) { if (cap) out[0] = 0; }
  template <class... A> void add(const char *fmt, A... args) {
    if (k_ < 0 || size_t(k_) >= cap_) return;
    const int m = std::snprintf(out_ + k_, cap_ - size_t(k_), fmt, args...);
    k_ = m < 0 ? -1 : k_ + m;
  }
  void str(const char *name, Reader &r, unsigned len) {
    char s[49]{};
    for (unsigned i = 0; i < len; ++i) { const char c = char(r.u8()); if (i < 48) s[i] = (c >= 32 && c < 127 && c != ',') ? c : '_'; }
    add(",%s=%s", name, s);
  }
  int size() const { return k_; }
 private:
  char *out_;
  size_t cap_;
  int k_ = 0;
};
inline const char *pidName(unsigned i) {
  static const char *names[] = {"0D", "0C", "05", "5B", "9A"};
  return i < 5 ? names[i] : "?";
}
inline int formatRecord(const uint8_t *rec, size_t n, char *out, size_t cap) {
  Formatter f(out, cap);
  if (n < 2 || size_t(rec[1]) + 2 != n) { f.add("%s", "type=bad"); return f.size(); }
  Reader r(rec + 2, n - 2);
  switch (rec[0]) {
    case RecBoot: {
      const unsigned long boot = r.u32(), seq = r.u32();
      const unsigned reset = r.u8(), rom = r.u8(), streak = r.u8(), fl = r.u8(), retained = r.u16();
      f.add("type=BOOT,boot=%lu,first_page_seq=%lu,reset=%u,rom_reset=%u,abnormal_streak=%u,"
            "store_ready=%u,boot_page_suppressed=%u,retained_pages=%u",
            boot, seq, reset, rom, streak, fl & 1U, (fl >> 1) & 1U, retained);
      f.str("git", r, 40);
      break;
    }
    case RecEvent: {
      static const char *names[] = {"?", "ARM", "DISARM", "MANUAL_STOP", "ECU_PIN", "ECU_UNPIN",
          "LINK_UP", "LINK_DOWN", "LINK_SUB", "LINK_UNSUB", "STOP_IDLE", "?", "RING_DROP",
          "FLUSH", "STORE_FAIL", "BOOT_SUPPRESSED", "AUTH_END", "ECU_STATE"};
      const unsigned long ms = r.u32();
      const unsigned code = r.u8(), a8 = r.u8(), a16 = r.u16();
      const unsigned long a32 = r.u32();
      f.add("type=EVENT,ms=%lu,event=%s,a8=%u,a16=%u,a32=%lu", ms, code < 18 ? names[code] : "?",
            a8, a16, a32);
      break;
    }
    case RecStop: {
      const unsigned long ms = r.u32(), epoch = r.u32();
      const unsigned fl = r.u8(), q = r.u8();
      const unsigned long src = r.u32(), acq = r.u32(), inv = r.u32();
      unsigned age[5];
      for (auto &a : age) a = r.u16();
      const unsigned st = r.u8(), resp = r.u8(), raw = r.u8(), ecu = r.u8(), bad = r.u8();
      const unsigned run = r.u16(), run_max = r.u16();
      f.add("type=STOP,ms=%lu,epoch=%lu,flags=0x%02X,last_quality=%u,source=%08lX,acquired=%lu,"
            "invalid=%lu,age_ms_0D=%u,age_ms_0C=%u,age_ms_05=%u,age_ms_5B=%u,age_ms_9A=%u,"
            "last_status=%u,last_responses=%u,last_raw=%u,ecu_state=%u,ecu_bad=%u,"
            "nodata_run=%u,nodata_run_max=%u",
            ms, epoch, fl, q, src, acq, inv, age[0], age[1], age[2], age[3], age[4], st, resp,
            raw, ecu, bad, run, run_max);
      f.str("reason", r, r.u8());
      f.str("can_last_error", r, r.u8());
      break;
    }
    case RecSummary: {
      const unsigned long ms = r.u32(), period = r.u32();
      const unsigned fl = r.u8(), q = r.u8();
      const unsigned long epoch = r.u32();
      f.add("type=SUMMARY,ms=%lu,period_ms=%lu,flags=0x%02X,last_quality=%u,epoch=%lu", ms,
            period, fl, q, epoch);
      for (unsigned i = 0; i < 5; ++i) {
        unsigned v[9];
        for (auto &x : v) x = r.u16();
        f.add(",%s=dem:%u/adm:%u/ok:%u/fail:%u/shed:%u/deny:%u/cancel:%u/samples:%u/maxgap_ms:%u",
              pidName(i), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
      }
      unsigned c[6];
      for (auto &x : c) x = r.u16();
      const unsigned ecu = r.u8(), bad = r.u8(), run = r.u16(), run_max = r.u16();
      f.add(",acquired=%u,invalid=%u,inv_nodata=%u,inv_verify=%u,inv_stop=%u,inv_other=%u,"
            "ecu_state=%u,ecu_bad=%u,nodata_run_win_max=%u,nodata_run_boot_max=%u",
            c[0], c[1], c[2], c[3], c[4], c[5], ecu, bad, run, run_max);
      break;
    }
    case RecSummary2: {
      const unsigned long ms = r.u32();
      unsigned q[10];
      for (auto &x : q) x = r.u16();
      f.add("type=SUMMARY2,ms=%lu,tx_attempts=%u,tx_success=%u,query=%u,query_ok=%u,"
            "query_timeout=%u,query_no_data=%u,query_failed=%u,v2_rx=%u,v2_fc=%u,app_queue_drops=%u",
            ms, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9]);
      f.add(",nrc=%u", unsigned(r.u16()));
      unsigned st[10];
      for (auto &x : st) x = r.u16();
      f.add(",st_ok=%u,st_nodata=%u,st_busy=%u,st_lease=%u,st_invalid=%u,st_rate=%u,st_driver=%u,"
            "st_busoff=%u,st_mode=%u,st_isotp=%u", st[0], st[1], st[2], st[3], st[4], st[5], st[6],
            st[7], st[8], st[9]);
      const unsigned miss = r.u16(), over = r.u16(), bus = r.u16(), state = r.u8(), depth = r.u8(),
                     fault = r.u8();
      f.add(",rx_missed=%u,rx_overrun=%u,bus_error=%u,twai_state=%u,rx_queue=%u,fault_locked=%u",
            miss, over, bus, state, depth, fault);
      unsigned g[11];
      for (auto &x : g) x = r.u16();
      const unsigned lop = r.u8(), lcode = r.u8();
      f.add(",data_pkts=%u,snapshot_pkts=%u,other_pkts=%u,snapshot_empty=%u,snapshot_records=%u,"
            "op4_ok=%u,op4_rej=%u,op7_ok=%u,op7_rej=%u,other_ok=%u,other_rej=%u,last_rej_op=%u,"
            "last_rej_code=%u", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10],
            lop, lcode);
      static const char *sites[] = {"transition", "stale", "control", "expired"};
      for (unsigned s = 0; s < 4; ++s) {
        const unsigned dd = r.u16(), ds = r.u16(), dx = r.u16();
        f.add(",discard_%s=data:%u/snapshot:%u/other:%u", sites[s], dd, ds, dx);
      }
      const unsigned results = r.u16();
      unsigned lat[ResultTap::kBuckets];
      for (auto &x : lat) x = r.u16();
      const unsigned multi = r.u16();
      f.add(",results=%u,lat_lt10=%u,lat_lt20=%u,lat_lt30=%u,lat_lt50=%u,lat_lt80=%u,lat_lt120=%u,"
            "lat_ge120=%u,lat_none=%u,multi_frame=%u", results, lat[0], lat[1], lat[2], lat[3],
            lat[4], lat[5], lat[6], lat[7], multi);
      const unsigned ls = r.u8(), lr = r.u8(), lw = r.u8(), sf = r.u8(), ff = r.u8(), cf = r.u8();
      const unsigned long first = r.u32(), ffus = r.u32(), cfus = r.u32();
      f.add(",last_status=%u,last_responses=%u,last_raw=%u,last_sf=%u,last_ff=%u,last_cf=%u,"
            "last_first_us=%lu,last_ff_us=%lu,last_cf_us=%lu", ls, lr, lw, sf, ff, cf, first, ffus,
            cfus);
      break;
    }
    case RecLive: {
      const unsigned long ms = r.u32(), epoch = r.u32();
      const unsigned fl = r.u8(), q = r.u8();
      const unsigned long acq = r.u32(), inv = r.u32(), src = r.u32();
      const unsigned long nd = r.u32(), vf = r.u32(), sp = r.u32(), ot = r.u32();
      f.add("type=LIVE,ms=%lu,epoch=%lu,flags=0x%02X,last_quality=%u,acquired=%lu,invalid=%lu,"
            "source=%08lX,inv_nodata=%lu,inv_verify=%lu,inv_stop=%lu,inv_other=%lu",
            ms, epoch, fl, q, acq, inv, src, nd, vf, sp, ot);
      for (unsigned i = 0; i < 5; ++i) {
        unsigned long v[8];
        for (auto &x : v) x = r.u32();
        const unsigned gap = r.u16();
        f.add(",%s=dem:%lu/adm:%lu/ok:%lu/fail:%lu/shed:%lu/deny:%lu/cancel:%lu/samples:%lu/maxgap_ms:%u",
              pidName(i), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], gap);
      }
      const unsigned long dp = r.u32(), sp2 = r.u32();
      const unsigned o4 = r.u16(), r4 = r.u16(), o7 = r.u16(), r7 = r.u16();
      const unsigned long drops = r.u32();
      const unsigned nodata = r.u16(), run_max = r.u16();
      f.add(",data_pkts=%lu,snapshot_pkts=%lu,op4_ok=%u,op4_rej=%u,op7_ok=%u,op7_rej=%u,"
            "ring_drops=%lu,nodata_results=%u,nodata_run_max=%u",
            dp, sp2, o4, r4, o7, r7, drops, nodata, run_max);
      break;
    }
    case RecBle: {
      const unsigned long ms = r.u32();
      const unsigned kind = r.u8(), known = r.u8(), conn = r.u16();
      const unsigned long gen = r.u32();
      const long reason = long(int32_t(r.u32()));
      const unsigned mtu = r.u16(), itv = r.u16(), lat = r.u16(), sup = r.u16(), code = r.u16();
      static const char *kinds[] = {"CONNECT", "DISCONNECT", "REJECT", "ERROR"};
      f.add("type=BLE,ms=%lu,kind=%s,conn=%u,gen=%lu,reason_known=%u,reason=%ld,mtu=%u,interval=%u,"
            "latency=%u,supervision=%u,code=%u", ms, kind < 4 ? kinds[kind] : "?", conn, gen, known,
            reason, mtu, itv, lat, sup, code);
      break;
    }
    case RecText: {
      const unsigned long ms = r.u32();
      const unsigned kind = r.u8();
      f.add("type=TEXT,ms=%lu,kind=%s", ms, kind == TextCanError ? "CAN_LAST_ERROR" : "?");
      f.str("text", r, r.u8());
      break;
    }
    default:
      f.add("type=unknown_%u,len=%u", unsigned(rec[0]), unsigned(rec[1]));
  }
  if (!r.ok) f.add("%s", ",truncated=1");
  return f.size();
}
}  // namespace recorder

#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)
#define M5CAN_V2REC_LINKED 1
}  // namespace signal_v2
namespace m5can { class CanMonitor; struct BleDiagnosticEvent; }
namespace signal_v2 {
namespace recorder {
// Device glue (src/signal_v2_flight_recorder.cpp).
// setup(), BEFORE CanMonitor::begin(): store scan + one boot page write.
void begin(m5can::CanMonitor *can, const char *sha, uint32_t reset, uint32_t rom_reset);
// BLE app owner task only (signal_v2::poll). Never blocks, no flash.
void observe(uint32_t now, const OwnerView &view, const PidDiagnostics &d, const CanSamples &s);
void control(uint8_t op, uint8_t result);
void produced(uint8_t type, uint8_t records);
void discarded(uint8_t site, uint8_t type);
void authEnd(uint32_t now, uint8_t code);
// Arduino loop only.
void bleEvent(const m5can::BleDiagnosticEvent &event);
void poll(m5can::CanMonitor &can);
bool usbCommand(const char *line);
}  // namespace recorder
#endif
}  // namespace signal_v2
