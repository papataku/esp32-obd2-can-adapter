#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "elm_compat.h"
#include "diagnostic_policy.h"

using namespace m5can;

int main() {
  ElmCompat elm;
  const auto caps = elm.execute("ATM5CAP");
  assert(caps.action == ElmAction::ReplyOnly);
#if defined(M5CAN_MULTI_ECU_POLICY_DRY_BENCH)
  // The *only* 1.2 implementation is a passive experiment. ECU inventory
  // cannot be confused with physical TX permission.
  assert(std::strstr(caps.reply, "PROTO=1.2") != nullptr);
  assert(std::strstr(caps.reply, "UDS_ECUS=01,02,06,0E,EF") != nullptr);
  assert(std::strstr(caps.reply, "TX=OFF") != nullptr);
#else
  // A production firmware build advertises original 1.1 byte-for-byte.
  assert(std::strstr(caps.reply, "PROTO=1.1") != nullptr);
  assert(std::strstr(caps.reply, "UDS_ECUS=") == nullptr);
  assert(std::strstr(caps.reply, "TX=OFF") == nullptr);
#endif

  for (auto source : {"01", "02", "06", "0E", "EF"}) {
    char command[24]{};
    std::snprintf(command, sizeof(command), "ATM5R12:%s:2012", source);
    const auto r = elm.execute(command);
#if defined(M5CAN_MULTI_ECU_POLICY_DRY_BENCH)
    assert(r.action == ElmAction::VehicleRead);
    assert(r.request.length == 3);
    assert(r.request.data[0] == 0x22);
    assert(r.request.data[1] == 0x20);
    assert(r.request.data[2] == 0x12);
    assert(DiagnosticPolicy::allowedReadRequest(
        DiagnosticRequest{r.request.can_id, r.request.length,
                          {r.request.data[0], r.request.data[1], r.request.data[2]},
                          200}));
#else
    assert(r.action == ElmAction::ReplyOnly);
    assert(std::strcmp(r.reply, "?") == 0);
#endif
  }

  for (const auto* command : {
      "ATM5R12:03:2012", "ATM5R12:FF:2012",
      "ATM5R12:02:201", "ATM5R12:02:20120",
      "ATM5R12:02:2G12", "ATM5R12:0G:2012",
      "ATM5R12:02;2012", "ATM5R12:02:2E2012",
      "ATM5R12:02:", "ATM5R12::2012",
      "ATM5R12:02:2012,2013", "ATM5R12:02:2012:01"
  }) {
    auto r = elm.execute(command);
    assert(r.action == ElmAction::ReplyOnly);
    assert(std::strcmp(r.reply, "?") == 0);
  }

  // Legacy 1.1 commands must be parsed byte-for-byte the same way.
  auto legacy = elm.execute("ATM5B01:2012");
  assert(legacy.action == ElmAction::BatchRead);
  assert(legacy.batch_count == 1);
  assert(legacy.batch[0].can_id == 0x18DA01F1U);
  assert(legacy.batch[0].data[0] == 0x22);
  legacy = elm.execute("ATM5B00:010C,010D,0105,015B,019A");
  assert(legacy.action == ElmAction::BatchRead);
  assert(legacy.batch_count == 5);
  assert(legacy.batch[0].can_id == 0x18DB33F1U);

  std::cout << "PASS optional explicit-ECU dry parser and legacy compatibility\n";
  return 0;
}
