// Host tests for the Signal V2 flight recorder (PM Claude 2026-11-15).
// Real EvidenceStore + recorder core over an in-memory 1.5 MiB "spiffs".
// Build: -DM5CAN_SIGNAL_V2_FLIGHT_RECORDER=1 (EvidenceStore V2Rec keys).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "evidence_store.h"
#include "signal_v2_flight_recorder.h"
using namespace signal_v2;
using namespace signal_v2::recorder;
using m5can::EvidenceStore;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

struct Flash final : m5can::EvidenceFlash {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x180000, 0xFF);
  std::vector<unsigned> erases = std::vector<unsigned>(0x180000 / 4096, 0);
  bool fail_write = false, armed = false, fail_erase = false, fail_write_only = false;
  unsigned writes = 0, erase_count = 0, armed_violations = 0;
  uint32_t size() const override { return uint32_t(bytes.size()); }
  bool read(uint32_t o, void *p, size_t n) override {
    if (o > bytes.size() || n > bytes.size() - o) return false;
    std::memcpy(p, bytes.data() + o, n); return true;
  }
  bool write(uint32_t o, const void *p, size_t n) override {
    if (armed) ++armed_violations;
    if (fail_write || fail_write_only || o > bytes.size() || n > bytes.size() - o) return false;
    const auto *s = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i) {
      if ((bytes[o + i] & s[i]) != s[i]) return false;  // NOR: 1->0 only
      bytes[o + i] &= s[i];
    }
    ++writes; return true;
  }
  bool erase(uint32_t o) override {
    if (armed) ++armed_violations;
    if (fail_write || fail_erase || o % 4096 || o + 4096 > bytes.size()) return false;
    std::fill_n(bytes.begin() + o, 4096, 0xFF);
    ++erases[o / 4096]; ++erase_count; return true;
  }
};
struct StoreAdapter final : PageStore {
  EvidenceStore &s;
  explicit StoreAdapter(EvidenceStore &x) : s(x) {}
  bool ready() const override { return s.status().ready; }
  bool put(unsigned slot, const uint8_t *d, size_t n) override { return s.put(EvidenceStore::V2Rec, slot, d, n); }
  size_t length(unsigned slot) const override { return s.length(EvidenceStore::V2Rec, slot); }
  size_t get(unsigned slot, uint8_t *d, size_t n) override { return s.get(EvidenceStore::V2Rec, slot, d, n); }
};
struct Cp final : Checkpoint {
  bool allow = true;
  unsigned begun = 0;
  bool begin() override { if (allow) ++begun; return allow; }
  void end() override {}
};
CanCounters g_can;
void readCan(CanCounters &c) { c = g_can; }
void readErr(char *out, size_t cap) { std::snprintf(out, cap, "%s", "running"); }

static std::vector<std::vector<uint8_t>> drainAll(SpscRing &ring) {
  std::vector<std::vector<uint8_t>> out;
  uint8_t b[260];
  for (size_t n; (n = ring.pop(b, sizeof(b)));) out.emplace_back(b, b + n);
  return out;
}

static void testRing() {
  SpscRing ring;
  uint8_t rec[200];
  for (unsigned i = 0; i < 200; ++i) rec[i] = uint8_t(i);
  rec[0] = RecEvent; rec[1] = 198;
  unsigned pushed = 0;
  while (ring.push(rec, 200)) ++pushed;
  CHECK(pushed == kRingBytes / 200);
  CHECK(ring.drops() == 1);
  for (unsigned round = 0; round < 50; ++round) {  // wraparound integrity
    uint8_t out[260];
    CHECK(ring.pop(out, sizeof(out)) == 200);
    CHECK(std::memcmp(out, rec, 200) == 0);
    CHECK(ring.push(rec, 200));
  }
  std::puts("PASS ring: bounded, drops counted, wraparound byte-exact");
}

struct World {
  PidDiagnostics d{};
  CanSamples s{};
  ResultTap tap{};
  OwnerView v{};
};

static void sample(World &w, uint64_t us) {
  for (unsigned i = 0; i < 5; ++i) {
    w.s.values[i].quality = 0; w.s.values[i].observed_us = us; ++w.s.values[i].sequence;
  }
  ++w.s.acquired;
}

static void testProducerRecords() {
  SpscRing ring;
  Producer p;
  p.attach(&ring, &readCan, &readErr);
  World w;
  w.v.epoch = 1;
  p.observe(0, w.v, w.d, w.s, w.tap);
  w.v.link[0] = true; w.v.subscribed[0] = true; w.v.generation[0] = 3;
  p.observe(10, w.v, w.d, w.s, w.tap);
  w.v.armed = true; w.v.epoch = 2;
  p.observe(2010, w.v, w.d, w.s, w.tap);
  w.s.source_id = 0x18DAF101u;
  w.tap.ecu_state = 1;
  p.observe(2200, w.v, w.d, w.s, w.tap);
  // 5 samples 200 ms apart, then one 900 ms gap.
  uint32_t t = 2400;
  for (unsigned k = 0; k < 5; ++k, t += 200) {
    w.d.pid[0].demand++; w.d.pid[0].admitted++; w.d.pid[0].completed++;
    sample(w, uint64_t(t) * 1000);
    p.observe(t, w.v, w.d, w.s, w.tap);
  }
  t += 700;
  sample(w, uint64_t(t) * 1000);
  p.observe(t, w.v, w.d, w.s, w.tap);
  w.s.invalidate(3); w.tap.status[1] = 4; w.tap.nodata_run = 4; w.tap.nodata_run_max = 4;
  p.control(4, 0); p.control(7, 7); p.produced(0x21, 0); p.produced(0x20, 5);
  p.discarded(DiscardSourceTransition, 0x20);
  p.observe(12100, w.v, w.d, w.s, w.tap);  // summary window elapsed
  auto recs = drainAll(ring);
  unsigned summaries = 0, summaries2 = 0, lives = 0;
  bool linkup = false, arm = false, pin = false;
  char text[1400];
  for (auto &r : recs) {
    if (r[0] == RecEvent) {
      linkup |= r[6] == EvLinkUp; arm |= r[6] == EvArm; pin |= r[6] == EvEcuPin;
    }
    if (r[0] == RecSummary) {
      ++summaries;
      formatRecord(r.data(), r.size(), text, sizeof(text));
      CHECK(std::strstr(text, "0D=dem:5/adm:5/ok:5/fail:0/shed:0/deny:0/cancel:0/samples:6/maxgap_ms:900"));
      CHECK(std::strstr(text, "inv_nodata=1"));
      CHECK(std::strstr(text, "nodata_run_boot_max=4"));
    }
    if (r[0] == RecSummary2) {
      ++summaries2;
      formatRecord(r.data(), r.size(), text, sizeof(text));
      CHECK(std::strstr(text, "snapshot_pkts=1,other_pkts=0,snapshot_empty=1"));
      CHECK(std::strstr(text, "op4_ok=1,op4_rej=0,op7_ok=0,op7_rej=1"));
      CHECK(std::strstr(text, "discard_transition=data:1/snapshot:0/other:0"));
      CHECK(std::strstr(text, "last_rej_op=7,last_rej_code=7"));
    }
    if (r[0] == RecLive) ++lives;
  }
  CHECK(linkup && arm && pin && summaries == 1 && summaries2 == 1 && lives >= 3);
  // Latched stop: summary, stop, live immediately; then STOP_IDLE once not busy.
  w.v.reason = "RX_ACCOUNTING_OR_BUS_FAULT"; w.v.stopped = true; w.v.busy = true; w.v.armed = false;
  p.observe(12150, w.v, w.d, w.s, w.tap);
  w.v.busy = false;
  p.observe(12160, w.v, w.d, w.s, w.tap);
  recs = drainAll(ring);
  bool stop = false, idle = false;
  for (auto &r : recs) {
    if (r[0] == RecStop) {
      stop = true;
      formatRecord(r.data(), r.size(), text, sizeof(text));
      CHECK(std::strstr(text, "reason=RX_ACCOUNTING_OR_BUS_FAULT"));
      CHECK(std::strstr(text, "can_last_error=running"));
      CHECK(std::strstr(text, "source=18DAF101"));
    }
    if (r[0] == RecEvent && r[6] == EvStopIdle) idle = true;
  }
  CHECK(stop && idle && p.latched());
  std::puts("PASS producer: link/arm/pin/ECU events, PID deltas, freshness gap 900ms, NoData run, ring ops, discards, latched STOP + STOP_IDLE");
}

// One simulated boot. Returns consumer status.
struct BootResult { Consumer::Status st; unsigned flash_writes_before_stop; };
static BootResult runBoot(Flash &flash, uint32_t reset, bool stop, unsigned summaries,
                          bool fail_at_stop = false, Cp *cp_override = nullptr) {
  static EvidenceStore store;
  CHECK(store.begin(flash, true));
  StoreAdapter adapter(store);
  static Consumer consumer;
  static SpscRing ring;
  drainAll(ring);
  Producer p;
  p.attach(&ring, &readCan, &readErr);
  BootInfo info;
  info.sha = "0123456789abcdef0123456789abcdef01234567";
  info.reset = reset;
  consumer.boot(&adapter, info, 0);
  Cp local;
  Cp &cp = cp_override ? *cp_override : local;
  World w;
  w.v.link[0] = w.v.subscribed[0] = true; w.v.armed = true; w.v.epoch = 5;
  flash.armed = true;  // TWAI running and armed: any flash access is a violation
  const unsigned before = flash.writes + flash.erase_count;
  uint32_t t = 100;
  for (unsigned k = 0; k < summaries * 50; ++k, t += 200) {
    w.d.pid[1].demand++; w.d.pid[1].admitted++; w.d.pid[1].completed++;
    sample(w, uint64_t(t) * 1000);
    p.observe(t, w.v, w.d, w.s, w.tap);
    consumer.drain(ring, t);
    consumer.flushStep(t, cp);
  }
  const unsigned during = flash.writes + flash.erase_count - before;
  if (stop) {
    w.v.reason = "QUERY_ADMISSION_FAILED"; w.v.stopped = true; w.v.armed = false;
    p.observe(t, w.v, w.d, w.s, w.tap);
    consumer.drain(ring, t);
    flash.armed = false;  // latched: TX permanently off until power cycle
    flash.fail_write = fail_at_stop;
    for (unsigned k = 0; k < 40; ++k, t += 50) {
      p.observe(t, w.v, w.d, w.s, w.tap);
      consumer.drain(ring, t);
      consumer.flushStep(t, cp);
    }
    flash.fail_write = false;
  }
  flash.armed = false;
  return {consumer.status(), during};
}

static void testRetentionAndOwnership() {
  Flash flash;
  // A foreign-but-owned old record (type Ble) plus a superseded revision.
  {
    EvidenceStore store;
    CHECK(store.begin(flash, true));
    uint8_t blob[100];
    std::memset(blob, 0xA5, sizeof(blob));
    CHECK(store.put(EvidenceStore::Ble, 0, blob, sizeof(blob)));
    blob[0] = 1;
    CHECK(store.put(EvidenceStore::Ble, 0, blob, sizeof(blob)));  // revision 2; rev 1 is stale
  }
  const std::vector<uint8_t> foreign_snapshot(flash.bytes.begin() + 2 * 4096, flash.bytes.begin() + 4 * 4096);
  unsigned total_puts = 0;
  uint32_t last_boot = 0;
  for (unsigned boot = 0; boot < 60; ++boot) {
    const bool stop = boot % 3 == 0;
    auto r = runBoot(flash, 1, stop, stop ? 40 : 3);
    CHECK(r.flash_writes_before_stop == 0);  // never touched flash while armed
    CHECK(flash.armed_violations == 0);
    CHECK(r.st.boot_written);
    CHECK(r.st.boot_id == last_boot + 1);
    last_boot = r.st.boot_id;
    CHECK(r.st.puts <= kMaxPutsPerBoot);
    if (stop) CHECK(r.st.puts >= 2 && r.st.window_open);
    else CHECK(r.st.puts == 1);
    total_puts += r.st.puts;
  }
  // Old foreign sectors (both revisions) survived: only blank/own sectors erased.
  CHECK(std::equal(foreign_snapshot.begin(), foreign_snapshot.end(), flash.bytes.begin() + 2 * 4096));
  EvidenceStore store;
  CHECK(store.begin(flash, false));
  StoreAdapter adapter(store);
  uint32_t min_seq = UINT32_MAX, max_seq = 0;
  unsigned pages = 0;
  uint8_t page[kPageBytes];
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    const size_t n = adapter.length(slot);
    if (!n) continue;
    CHECK(adapter.get(slot, page, n) == n);
    PageHeader h{};
    CHECK(parseHeader(page, n, h));
    CHECK(h.page_seq % kSlots == slot);
    min_seq = std::min(min_seq, h.page_seq); max_seq = std::max(max_seq, h.page_seq);
    ++pages;
  }
  CHECK(pages == kSlots && max_seq == total_puts && max_seq - min_seq + 1 == kSlots);
  unsigned max_erase = 0;
  for (unsigned e : flash.erases) max_erase = std::max(max_erase, e);
  std::printf("PASS retention: 60 boots, %u puts, last %u pages retained (seq %u..%u), max erases/sector=%u, foreign Ble sectors untouched, 0 armed writes\n",
              total_puts, pages, min_seq, max_seq, max_erase);
}

static void testFailureInjection() {
  Flash flash;
  auto ok = runBoot(flash, 1, false, 1);
  CHECK(ok.st.boot_written);
  // Store write fails at the stop flush: recorder disables itself, no retries.
  auto r = runBoot(flash, 1, true, 2, true);
  CHECK(r.st.store_failed && r.st.put_failures == 1);
  CHECK(r.st.puts == 2);  // boot page + exactly one failed attempt
  // Fails at boot: nothing more for that boot.
  {
    static EvidenceStore store;
    CHECK(store.begin(flash, true));
    StoreAdapter adapter(store);
    Consumer c;
    flash.fail_write = true;
    BootInfo info;
    c.boot(&adapter, info, 0);
    flash.fail_write = false;
    CHECK(c.status().store_failed && !c.status().boot_written);
  }
  // Busy CAN checkpoint: no write while a transaction owns the bus.
  Cp busy;
  busy.allow = false;
  const unsigned before = flash.writes;
  auto b = runBoot(flash, 1, true, 1, false, &busy);
  CHECK(b.st.puts == 1 && b.st.window_open && flash.writes > before);  // only the boot page
  // Store unavailable (foreign partition): RAM only, never writes.
  Flash foreign;
  foreign.bytes[5000] = 0x00;  // not blank, no owner header
  EvidenceStore fs;
  CHECK(!fs.begin(foreign, true) && fs.status().foreign);
  StoreAdapter fa(fs);
  Consumer fc;
  BootInfo info;
  fc.boot(&fa, info, 0);
  CHECK(!fc.status().store_ready && foreign.writes == 0 && foreign.erase_count == 0);
  std::puts("PASS failure injection: write failure disables persistence, busy checkpoint defers, foreign store never written");
}

static void testBootLoopLimiter() {
  Flash flash;
  unsigned written = 0, suppressed = 0;
  for (unsigned i = 0; i < 20; ++i) {
    auto r = runBoot(flash, 4 /*PANIC*/, false, 0);
    written += r.st.boot_written; suppressed += r.st.boot_suppressed;
  }
  CHECK(written == kAbnormalStreakLimit && suppressed == 20 - written);
  auto r = runBoot(flash, 1 /*POWERON*/, false, 0);
  CHECK(r.st.boot_written && r.st.streak == 0);
  std::printf("PASS boot-loop limiter: %u panic boot pages then suppressed; power-on resumes\n", written);
}

static void testEvictionPolicy() {
  Consumer c;
  BootInfo info;
  c.boot(nullptr, info, 0);
  uint8_t rec[202];
  rec[0] = RecEvent; rec[1] = 200;
  std::memset(rec + 2, 0x11, 200);
  for (unsigned i = 0; i < 200; ++i) c.append(rec, sizeof(rec));
  CHECK(c.status().dropped_evicted > 0 && c.status().dropped_post == 0);
  CHECK(c.ramPages() == kRamPages);
  CHECK(c.ramPage(0).flags & PageHead);
  bool overflow = false;
  for (unsigned k = 1; k < c.ramPages(); ++k) overflow |= (c.ramPage(k).flags & PageOverflow) != 0;
  CHECK(overflow);
  std::puts("PASS RAM journal: pinned head page, oldest tail evicted before stop with overflow flag");
}


// ---- Reclaim of old BLE-journal (type 3) sectors, owner decision 2026-11-15 ----
// Store header layout (flash ABI): magic u32, version u16, type u16, seq u64,
// slot u32, bytes u32, crc u32, commit u32.
static uint16_t typeAt(const Flash &f, unsigned sector) {
  uint16_t t; std::memcpy(&t, f.bytes.data() + sector * 4096 + 6, 2); return t;
}
static uint32_t magicAt(const Flash &f, unsigned sector) {
  uint32_t m; std::memcpy(&m, f.bytes.data() + sector * 4096, 4); return m;
}
static uint64_t seqAt(const Flash &f, unsigned sector) {
  uint64_t q; std::memcpy(&q, f.bytes.data() + sector * 4096 + 8, 8); return q;
}
static constexpr unsigned kSectors = 0x180000 / 4096;
static constexpr unsigned kSpecial = 7;  // sectors 2..6 hold non-reclaimable records
// Bench layout: owner x2 + every data sector a committed record. Sectors 2..6
// carry the OLDEST sequences but must never be reclaimed: Probe (other type),
// CRC-corrupt Ble, uncommitted Ble, unknown type 9, CRC-corrupt Ble with a
// valid-looking header. Sectors 7..383 hold CRC-valid Ble revisions.
static void fillFullBle(Flash &flash) {
  EvidenceStore store;
  CHECK(store.begin(flash, true));
  uint8_t blob[100];
  for (unsigned i = 0; i < kSectors - 2; ++i) {
    std::memset(blob, int(i & 0xFF), sizeof(blob));
    const auto t = i == 0 ? EvidenceStore::Probe : EvidenceStore::Ble;
    CHECK(store.put(t, i % 4, blob, sizeof(blob)));
  }
  for (unsigned s = 2; s < kSectors; ++s) CHECK(magicAt(flash, s) == 0x4D354552u);
  flash.bytes[3 * 4096 + 40] ^= 0x01;                                     // CRC corrupt
  std::memset(flash.bytes.data() + 4 * 4096 + 28, 0xFF, 4);               // uncommitted
  flash.bytes[5 * 4096 + 6] = 9;                                          // unknown type
  flash.bytes[6 * 4096 + 32 + 99] ^= 0x80;                                // CRC corrupt (tail)
  CHECK(typeAt(flash, 2) == EvidenceStore::Probe);
  EvidenceStore check;
  CHECK(check.begin(flash, false));
  CHECK(check.status().corrupt == 3 && check.status().uncommitted == 1);
}
static unsigned countType(const Flash &f, uint16_t type) {
  unsigned n = 0;
  for (unsigned s = 2; s < kSectors; ++s) n += magicAt(f, s) == 0x4D354552u && typeAt(f, s) == type;
  return n;
}
// Sector currently holding the oldest Ble revision that is not the latest of its key.
static unsigned expectedReclaim(const Flash &f) {
  uint64_t latest[4] = {0, 0, 0, 0};
  for (unsigned s = kSpecial; s < kSectors; ++s)
    if (magicAt(f, s) == 0x4D354552u && typeAt(f, s) == EvidenceStore::Ble) {
      uint32_t slot; std::memcpy(&slot, f.bytes.data() + s * 4096 + 16, 4);
      latest[slot] = std::max(latest[slot], seqAt(f, s));
    }
  unsigned best = 0; uint64_t best_seq = UINT64_MAX;
  for (unsigned s = kSpecial; s < kSectors; ++s)
    if (magicAt(f, s) == 0x4D354552u && typeAt(f, s) == EvidenceStore::Ble) {
      const uint64_t q = seqAt(f, s);
      bool is_latest = false;
      for (uint64_t l : latest) is_latest |= l == q;
      if (!is_latest && q < best_seq) { best_seq = q; best = s; }
    }
  return best;
}

static void testReclaimFullBleStore() {
  Flash flash;
  fillFullBle(flash);
  const std::vector<uint8_t> keep(flash.bytes.begin(), flash.bytes.begin() + kSpecial * 4096);
  const unsigned ble0 = countType(flash, EvidenceStore::Ble);
  std::vector<unsigned> order;
  // PR #77 behaviour on this image was boot_page=failed. Now: one reclaim per boot page.
  for (unsigned boot = 1; boot <= 49; ++boot) {
    const unsigned want = expectedReclaim(flash);
    CHECK(want >= kSpecial);
    const auto erases = flash.erases;
    auto r = runBoot(flash, 1, false, 0);
    CHECK(r.st.boot_written && !r.st.store_failed && r.st.puts == 1 && r.st.put_failures == 0);
    unsigned erased = 0, which = 0;
    for (unsigned s = 0; s < kSectors; ++s) if (flash.erases[s] != erases[s]) { ++erased; which = s; }
    CHECK(erased == 1 && which == want);                 // exactly one, oldest first
    CHECK(typeAt(flash, which) == EvidenceStore::V2Rec);
    order.push_back(which);
    CHECK(countType(flash, EvidenceStore::Ble) == ble0 - boot);
    CHECK(countType(flash, EvidenceStore::V2Rec) == boot);
  }
  for (size_t i = 1; i < order.size(); ++i) CHECK(seqAt(flash, order[i]) > seqAt(flash, order[i - 1]));
  // 48 pages + 1 commit spare reached: V2Rec now rotates within its own sectors.
  const std::vector<uint8_t> ble_before = flash.bytes;
  for (unsigned boot = 0; boot < 40; ++boot) {
    auto r = runBoot(flash, 1, boot % 4 == 0, boot % 4 == 0 ? 20 : 0);
    CHECK(r.st.boot_written && !r.st.store_failed && r.flash_writes_before_stop == 0);
  }
  CHECK(countType(flash, EvidenceStore::Ble) == ble0 - 49 && countType(flash, EvidenceStore::V2Rec) == 49);
  for (unsigned s = 0; s < kSectors; ++s)
    if (!(magicAt(flash, s) == 0x4D354552u && typeAt(flash, s) == EvidenceStore::V2Rec))
      CHECK(std::equal(ble_before.begin() + s * 4096, ble_before.begin() + (s + 1) * 4096, flash.bytes.begin() + s * 4096));
  CHECK(std::equal(keep.begin(), keep.end(), flash.bytes.begin()));  // owner + Probe/corrupt/uncommitted/unknown
  CHECK(flash.armed_violations == 0);
  EvidenceStore store;
  CHECK(store.begin(flash, false));
  StoreAdapter a(store);
  unsigned pages = 0;
  for (unsigned slot = 0; slot < kSlots; ++slot) pages += a.length(slot) != 0;
  CHECK(pages == kSlots);
  for (unsigned k = 0; k < 4; ++k) CHECK(store.length(EvidenceStore::Ble, k) == 100);  // latest per key kept
  std::printf("PASS reclaim: full Ble store, 49 boots reclaimed one oldest Ble sector each (sectors %u..%u), "
              "then 40 boots with 0 Ble erases; 48 pages + 1 spare; owner/Probe/corrupt/uncommitted/unknown untouched\n",
              order.front(), order.back());
}

static void testReclaimOnlyAsNeeded() {
  Flash flash;
  fillFullBle(flash);
  const unsigned ble0 = countType(flash, EvidenceStore::Ble);
  const auto fill_erases = flash.erases;
  auto r = runBoot(flash, 1, true, 40);  // boot page + latched-stop flush pages
  CHECK(r.flash_writes_before_stop == 0 && flash.armed_violations == 0);
  CHECK(r.st.puts >= 2 && r.st.puts <= kMaxPutsPerBoot && !r.st.store_failed);
  CHECK(countType(flash, EvidenceStore::V2Rec) == r.st.puts);
  CHECK(countType(flash, EvidenceStore::Ble) == ble0 - r.st.puts);
  unsigned erased = 0;
  for (unsigned k = 0; k < kSectors; ++k) erased += flash.erases[k] - fill_erases[k];
  CHECK(erased == r.st.puts);  // one reclaim (erase) per page write, no more
  std::printf("PASS reclaim: stop boot wrote %u pages, reclaimed exactly %u Ble sectors\n", r.st.puts, erased);
}

static void testReclaimFailure() {
  {  // Erase fails at boot: sector intact, persistence off for this boot.
    Flash flash;
    fillFullBle(flash);
    const std::vector<uint8_t> before = flash.bytes;
    flash.fail_erase = true;
    auto r = runBoot(flash, 1, true, 3);
    flash.fail_erase = false;
    CHECK(!r.st.boot_written && r.st.store_failed && r.st.puts == 1 && r.st.put_failures == 1);
    CHECK(flash.bytes == before);
    CHECK(r.flash_writes_before_stop == 0 && flash.armed_violations == 0);
    auto next = runBoot(flash, 1, false, 0);  // next boot retries once
    CHECK(next.st.boot_written && countType(flash, EvidenceStore::V2Rec) == 1);
  }
  {  // Write fails after the erase: that old Ble revision is gone, nothing else.
    Flash flash;
    fillFullBle(flash);
    const unsigned want = expectedReclaim(flash);
    const unsigned ble0 = countType(flash, EvidenceStore::Ble);
    flash.fail_write_only = true;
    auto r = runBoot(flash, 1, true, 3);
    flash.fail_write_only = false;
    CHECK(!r.st.boot_written && r.st.store_failed && r.st.puts == 1);
    CHECK(magicAt(flash, want) == UINT32_MAX && countType(flash, EvidenceStore::Ble) == ble0 - 1);
    CHECK(countType(flash, EvidenceStore::V2Rec) == 0);
  }
  {  // Failure inside the latched-stop flush (reclaim path): no further puts that boot.
    Flash flash;
    fillFullBle(flash);
    auto r = runBoot(flash, 1, true, 3, true);
    CHECK(r.st.boot_written && r.st.store_failed && r.st.puts == 2 && r.st.put_failures == 1);
    CHECK(r.flash_writes_before_stop == 0 && flash.armed_violations == 0);
  }
  std::puts("PASS reclaim failure: erase/write failure disables persistence for the boot; erase failure leaves Ble intact");
}

static void writeMixedImage(const char *path) {
  Flash flash;
  fillFullBle(flash);
  runBoot(flash, 1, false, 1);
  runBoot(flash, 9 /*BROWNOUT*/, true, 4);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(flash.bytes.data()),
                                              std::streamsize(flash.bytes.size()));
  std::printf("v2rec=%u ble=%u\n", countType(flash, EvidenceStore::V2Rec), countType(flash, EvidenceStore::Ble));
}

static void writeImage(const char *path) {
  Flash flash;
  runBoot(flash, 1, false, 1);
  runBoot(flash, 9 /*BROWNOUT*/, true, 4);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(flash.bytes.data()),
                                              std::streamsize(flash.bytes.size()));
  // Print the same pages as USB DUMP text for decoder cross-checks.
  EvidenceStore store;
  CHECK(store.begin(flash, false));
  StoreAdapter a(store);
  uint8_t page[kPageBytes];
  char line[1400];
  for (unsigned slot = 0; slot < kSlots; ++slot) {
    const size_t n = a.length(slot);
    if (!n || a.get(slot, page, n) != n) continue;
    PageHeader h{};
    CHECK(parseHeader(page, n, h));
    size_t at = kPageHeader;
    while (at + 2 <= kPageHeader + h.payload) {
      const size_t m = size_t(page[at + 1]) + 2;
      formatRecord(page + at, m, line, sizeof(line));
      std::printf("#V2REC,src=flash,page_seq=%u,boot=%u,%s\n", h.page_seq, h.boot_id, line);
      at += m;
    }
  }
}

int main(int argc, char **argv) {
  if (argc == 3 && !std::strcmp(argv[1], "image")) { writeImage(argv[2]); return 0; }
  if (argc == 3 && !std::strcmp(argv[1], "mixed")) { writeMixedImage(argv[2]); return 0; }
  testRing();
  testProducerRecords();
  testRetentionAndOwnership();
  testFailureInjection();
  testBootLoopLimiter();
  testEvictionPolicy();
  testReclaimFullBleStore();
  testReclaimOnlyAsNeeded();
  testReclaimFailure();
  std::puts("PASS V2 flight recorder host tests");
}
