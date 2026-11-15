// Signal V2 flight recorder device glue (PM Claude 2026-11-15).
// Compiled only with M5CAN_SIGNAL_V2_FLIGHT_RECORDER (owner_live env).
// Never transmits, never selects/admits/reserves CAN traffic, never writes
// flash outside: setup() before TWAI start, or the Arduino loop after a
// latched owner stop under CanMonitor's CAN-idle log checkpoint.
#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)
#include "signal_v2_flight_recorder.h"
#include <Arduino.h>
#include <atomic>
#include "ble_elm_transport.h"
#include "can_monitor.h"
#include "evidence_store.h"

#if !defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE) || !defined(M5CAN_SIGNAL_V2_OWNER_AUTO_ARM) || \
    !defined(M5CAN_DUAL_BLE_EXPERIMENTAL) || defined(M5CAN_SIGNAL_V2_SERIAL_HIL) || \
    defined(M5CAN_SIGNAL_V2_STAGE_GATED)
#error "flight recorder requires the owner_live image (latched AUTO_ARM stop, EvidenceStore)"
#endif

namespace signal_v2 {
namespace recorder {
static_assert(m5can::EvidenceStore::kV2RecSlots == kSlots, "slot count ABI");
static_assert(m5can::EvidenceStore::kMaxBlob == kPageBytes, "page size ABI");
namespace {
m5can::CanMonitor *g_can = nullptr;
SpscRing g_ring;
Producer g_producer;  // BLE app owner task only
Consumer g_consumer;  // Arduino loop only (and setup before tasks start)
char g_line[1400];

void readCan(CanCounters &out) {
  if (!g_can) return;
  static m5can::CanStats s;  // BLE app task only: keep it off the task stack
  g_can->stats(s);
  out.tx_attempts = s.tx_attempts; out.tx_success = s.tx_success;
  out.query_count = s.query_count; out.query_ok = s.query_ok;
  out.query_timeout = s.query_timeout; out.query_no_data = s.query_no_data;
  out.query_failed = s.query_failed; out.v2_rx = s.signal_v2_rx; out.v2_fc = s.signal_v2_fc;
  out.nrc = s.nrc_count;
  for (unsigned i = 0; i < 10; ++i) out.status[i] = s.result_status_count[i];
  out.rx_missed = s.driver_rx_missed; out.rx_overrun = s.driver_rx_overrun;
  out.bus_error = s.driver_bus_error; out.rx_queue_depth = s.rx_queue_depth;
  out.state = static_cast<uint8_t>(s.state);
  out.app_queue_drops = s.app_queue_drops;
  out.fault_locked = g_can->faultLocked();
}
void readError(char *out, size_t cap) {
  // Diagnostic text only; a concurrent CAN-owner update may garble it.
  if (!g_can || !cap) return;
  const char *e = g_can->lastError();
  size_t i = 0;
  for (; e && i + 1 < cap && e[i]; ++i) out[i] = e[i];
  out[i] = 0;
}
// Published by the producer each tick: CAN currently armed and not stopped.
std::atomic<bool> g_can_live{false};

class Store final : public PageStore {
 public:
  bool ready() const override { return m5can::evidenceStore().status().ready; }
  bool put(unsigned slot, const uint8_t *data, size_t bytes) override {
    return m5can::evidenceStore().put(m5can::EvidenceStore::V2Rec, slot, data, bytes);
  }
  size_t length(unsigned slot) const override {
    return m5can::evidenceStore().length(m5can::EvidenceStore::V2Rec, slot);
  }
  size_t get(unsigned slot, uint8_t *data, size_t bytes) override {
    return m5can::evidenceStore().get(m5can::EvidenceStore::V2Rec, slot, data, bytes);
  }
} g_store;

class CanIdle final : public Checkpoint {
 public:
  explicit CanIdle(m5can::CanMonitor &can) : can_(can) {}
  bool begin() override { return can_.tryBeginLogCheckpoint(); }
  void end() override { can_.endLogCheckpoint(); }
 private:
  m5can::CanMonitor &can_;
};

// USB DUMP: one page per loop iteration; read-only, no CAN checkpoint.
struct Dump {
  bool active = false, hex = false;
  unsigned count = 0, next = 0, ram_next = 0;
  uint32_t seq[kSlots]{};
  uint8_t slot[kSlots]{};
} g_dump;
uint8_t g_page[kPageBytes];

void printRecords(const char *src, const PageHeader &h, const uint8_t *payload, size_t used) {
  size_t at = 0;
  unsigned i = 0;
  while (at + 2 <= used) {
    const size_t n = size_t(payload[at + 1]) + 2;
    if (at + n > used) break;
    const int k = std::snprintf(g_line, sizeof(g_line), "#V2REC,src=%s,page_seq=%lu,boot=%lu,i=%u,",
                                src, (unsigned long)h.page_seq, (unsigned long)h.boot_id, i++);
    if (k > 0 && size_t(k) < sizeof(g_line))
      formatRecord(payload + at, n, g_line + k, sizeof(g_line) - size_t(k));
    Serial.println(g_line);
    at += n;
  }
}
void printPage(const char *src, unsigned slot, const uint8_t *page, size_t n) {
  PageHeader h{};
  if (!parseHeader(page, n, h)) {
    Serial.printf("#V2REC_PAGE,src=%s,slot=%u,valid=0\n", src, slot);
    return;
  }
  Serial.printf("#V2REC_PAGE,src=%s,slot=%u,page_seq=%lu,boot=%lu,kind=%s,page_in_boot=%u,"
                "records=%u,bytes=%u,flags=0x%X,dropped=%lu\n",
                src, slot, (unsigned long)h.page_seq, (unsigned long)h.boot_id,
                h.kind == BootPage ? "BOOT" : "FLUSH", h.page_in_boot, h.records, h.payload,
                h.flags, (unsigned long)h.dropped);
  printRecords(src, h, page + kPageHeader, h.payload);
  if (!g_dump.hex) return;
  const size_t total = kPageHeader + h.payload;
  for (size_t off = 0; off < total; off += 64) {
    int k = std::snprintf(g_line, sizeof(g_line), "#V2REC_HEX,src=%s,page_seq=%lu,off=%u,data=",
                          src, (unsigned long)h.page_seq, unsigned(off));
    for (size_t j = off; j < total && j < off + 64 && k > 0 && size_t(k) + 3 < sizeof(g_line); ++j)
      k += std::snprintf(g_line + k, sizeof(g_line) - size_t(k), "%02X", page[j]);
    Serial.println(g_line);
  }
}
void startDump(bool hex) {
  g_dump = Dump{};
  g_dump.active = true;
  g_dump.hex = hex;
  if (g_store.ready()) {
    for (unsigned s = 0; s < kSlots; ++s) {
      const size_t n = g_store.length(s);
      PageHeader h{};
      if (!n || n > kPageBytes || g_store.get(s, g_page, n) != n || !parseHeader(g_page, n, h))
        continue;
      unsigned at = g_dump.count++;
      while (at && g_dump.seq[at - 1] > h.page_seq) {
        g_dump.seq[at] = g_dump.seq[at - 1]; g_dump.slot[at] = g_dump.slot[at - 1]; --at;
      }
      g_dump.seq[at] = h.page_seq; g_dump.slot[at] = uint8_t(s);
    }
  }
  Serial.printf("#V2REC_DUMP,begin,flash_pages=%u,ram_pages=%u,read_only=1\n", g_dump.count,
                g_consumer.ramPages());
}
void dumpStep() {
  if (!g_dump.active) return;
  if (g_can_live.load(std::memory_order_relaxed)) {
    // PM 2026-11-15: read-back only while CAN is idle (disarmed or latched stop).
    g_dump.active = false;
    Serial.println("#V2REC_DUMP,aborted=CAN_ARMED,read_only=1");
    return;
  }
  if (g_dump.next < g_dump.count) {
    const unsigned s = g_dump.slot[g_dump.next++];
    const size_t n = g_store.length(s);
    if (n && n <= kPageBytes && g_store.get(s, g_page, n) == n) printPage("flash", s, g_page, n);
    else Serial.printf("#V2REC_PAGE,src=flash,slot=%u,valid=0\n", s);
    return;
  }
  if (g_dump.ram_next < g_consumer.ramPages()) {
    const auto &p = g_consumer.ramPage(g_dump.ram_next++);
    const auto &st = g_consumer.status();
    const size_t n = Consumer::encodePage(g_page, FlushPage, p.page_in_boot, p.bytes, p.used,
                                          p.records, p.flags, p.dropped_before, 0, st.boot_id);
    Serial.printf("#V2REC_RAM,page_in_boot=%u,persisted_bytes=%u,used=%u\n", p.page_in_boot,
                  p.flushed, p.used);
    if (n) printPage("ram", 0xFF, g_page, n);
    return;
  }
  g_dump.active = false;
  Serial.println("#V2REC_DUMP,end");
}
void printStatus() {
  const auto &st = g_consumer.status();
  const auto &ev = m5can::evidenceStore().status();
  size_t unflushed = 0;
  for (unsigned k = 0; k < g_consumer.ramPages(); ++k) {
    const auto &p = g_consumer.ramPage(k);
    unflushed += size_t(p.used - p.flushed);
  }
  Serial.printf("#V2REC_STATUS,compiled=1,uptime_ms=%lu,store_ready=%u,store_failed=%u,boot=%lu,"
                "next_page_seq=%lu,retained_at_boot=%lu,boot_page=%s,puts=%lu,put_failures=%lu,"
                "window_open=%u,ram_pages=%u,ram_unpersisted_bytes=%u,records=%lu,"
                "dropped_evicted=%lu,dropped_post=%lu,ring_drops=%lu,evidence_sectors=%lu,"
                "evidence_corrupt=%lu,evidence_foreign=%u,slots=%u,max_puts_per_boot=%u\n",
                (unsigned long)millis(), st.store_ready ? 1U : 0U, st.store_failed ? 1U : 0U,
                (unsigned long)st.boot_id, (unsigned long)st.next_page_seq,
                (unsigned long)st.retained,
                st.boot_written ? "written" : st.boot_suppressed ? "suppressed"
                    : st.store_ready ? "failed" : "unavailable",
                (unsigned long)st.puts, (unsigned long)st.put_failures, st.window_open ? 1U : 0U,
                g_consumer.ramPages(), unsigned(unflushed), (unsigned long)st.records,
                (unsigned long)st.dropped_evicted, (unsigned long)st.dropped_post,
                (unsigned long)g_ring.drops(), (unsigned long)ev.sectors,
                (unsigned long)ev.corrupt, ev.foreign ? 1U : 0U, kSlots, kMaxPutsPerBoot);
  size_t n = 0;
  const uint8_t *live = g_consumer.live(n);
  if (n) {
    const int k = std::snprintf(g_line, sizeof(g_line), "#V2REC_LIVE,");
    formatRecord(live, n, g_line + k, sizeof(g_line) - size_t(k));
    Serial.println(g_line);
  } else {
    Serial.println("#V2REC_LIVE,none=1");
  }
}
}  // namespace

void begin(m5can::CanMonitor *can, const char *sha, uint32_t reset, uint32_t rom_reset) {
  g_can = can;
  g_producer.attach(&g_ring, &readCan, &readError);
  // Read-only scan; a blank partition gets only the existing owner header,
  // foreign or encrypted data fails closed (EvidenceStore::begin).
  m5can::initializeEvidenceStore();
  BootInfo info;
  info.sha = sha; info.reset = reset; info.rom_reset = rom_reset;
  g_consumer.boot(&g_store, info, millis());
  const auto &st = g_consumer.status();
  Serial.printf("#V2REC,boot=%lu,store_ready=%u,boot_page=%s,next_page_seq=%lu,retained=%lu\n",
                (unsigned long)st.boot_id, st.store_ready ? 1U : 0U,
                st.boot_written ? "written" : st.boot_suppressed ? "suppressed"
                    : st.store_ready ? "failed" : "unavailable",
                (unsigned long)st.next_page_seq, (unsigned long)st.retained);
}
void observe(uint32_t now, const OwnerView &view, const PidDiagnostics &d, const CanSamples &s) {
  g_can_live.store(view.armed && !view.stopped, std::memory_order_relaxed);
  g_producer.observe(now, view, d, s, resultTap());
}
void control(uint8_t op, uint8_t result) { g_producer.control(op, result); }
void produced(uint8_t type, uint8_t records) { g_producer.produced(type, records); }
void discarded(uint8_t site, uint8_t type) { g_producer.discarded(site, type); }
void authEnd(uint32_t now, uint8_t code) { g_producer.authEnd(now, code); }
void bleEvent(const m5can::BleDiagnosticEvent &e) {
  // Deliberately copies no peer address bytes, traffic byte counts or identity.
  uint8_t b[26];
  Writer w(b, sizeof(b));
  w.u8(RecBle); w.u8(24); w.u32(e.uptime_ms); w.u8(static_cast<uint8_t>(e.kind));
  w.u8(e.reason_known ? 1 : 0); w.u16(e.connection_id); w.u32(e.generation);
  w.u32(static_cast<uint32_t>(e.reason)); w.u16(e.mtu); w.u16(e.interval);
  w.u16(e.latency); w.u16(e.supervision); w.u16(e.error_code);
  if (w.ok) g_consumer.append(b, w.n);
}
void poll(m5can::CanMonitor &can) {
  const uint32_t now = millis();
  g_consumer.drain(g_ring, now);
  CanIdle idle(can);
  g_consumer.flushStep(now, idle);
  dumpStep();
}
bool usbCommand(const char *line) {
  if (std::strncmp(line, "V2REC", 5) || (line[5] && line[5] != ' ')) return false;
  if (!std::strcmp(line, "V2REC STATUS") || !std::strcmp(line, "V2REC")) {
    printStatus();
  } else if (!std::strcmp(line, "V2REC DUMP") || !std::strcmp(line, "V2REC DUMP HEX")) {
    if (g_dump.active) Serial.println("#V2REC_DUMP,busy=1");
    else if (g_can_live.load(std::memory_order_relaxed))
      Serial.println("#V2REC_DUMP,refused=CAN_ARMED,hint=stop_or_disconnect_first,read_only=1");
    else startDump(!std::strcmp(line, "V2REC DUMP HEX"));
  } else {
    Serial.println("#V2REC,help=V2REC STATUS|V2REC DUMP|V2REC DUMP HEX,clear=unsupported,read_only=1");
  }
  return true;
}
}  // namespace recorder
}  // namespace signal_v2
#endif
