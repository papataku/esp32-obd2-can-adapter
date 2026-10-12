#include <cassert>
#include <cstdint>
#include "../src/diagnostic_result_cache.h"

using namespace m5can;

int main() {
  DiagnosticResultCache cache;
  DiagnosticRequest speed{};
  speed.can_id = 0x18DB33F1U;
  speed.length = 2;
  speed.data[0] = 0x01;
  speed.data[1] = 0x0D;

  DiagnosticResult valid{};
  valid.status = DiagnosticStatus::Ok;
  valid.response_count = 1;
  valid.responses[0].can_id = 0x18DAF101U;
  valid.responses[0].length = 3;
  valid.responses[0].data[0] = 0x41;
  valid.responses[0].data[1] = 0x0D;
  valid.responses[0].data[2] = 42;
  valid.raw_frame_count = 1;
  valid.transaction_us = 78000;

  DiagnosticResult reused{};
  assert(!cache.lookup(speed, 500000, reused));
  cache.remember(speed, valid, 500000);
  assert(cache.lookup(speed, 500000, reused));
  assert(reused.responses[0].data[2] == 42);
  assert(reused.transaction_us == 0);
  assert(cache.lookup(speed, 599999, reused));  // within 100ms
  assert(!cache.lookup(speed, 600000, reused)); // 100ms itself is expired
  assert(!cache.lookup(speed, 499999, reused)); // clock rollback

  DiagnosticRequest rpm = speed;
  rpm.data[1] = 0x0c;
  assert(!cache.lookup(rpm, 501000, reused));
  DiagnosticRequest other_header = speed;
  other_header.can_id = 0x18DBEFF1U;
  assert(!cache.lookup(other_header, 501000, reused));

  // An ECU negative reply must not become a cached meter value, even if
  // DiagnosticPolicy recognizes its service number.
  DiagnosticResult negative = valid;
  negative.responses[0].data[0] = 0x7f;
  negative.responses[0].data[1] = 0x01;
  negative.responses[0].data[2] = 0x22;
  cache.remember(speed, negative, 510000);
  assert(!cache.lookup(speed, 510001, reused));

  cache.remember(speed, valid, 520000);
  cache.remember(speed, DiagnosticResult{}, 530000);
  assert(!cache.lookup(speed, 530001, reused)); // invalidate on failure

  DiagnosticRequest unsafe = speed;
  unsafe.data[0] = 0x2e; // write-by-identifier is prohibited
  cache.remember(unsafe, valid, 540000);
  assert(!cache.lookup(unsafe, 540001, reused));

  cache.remember(speed, valid, 550000);
  cache.clear();
  assert(!cache.lookup(speed, 550001, reused));
}
