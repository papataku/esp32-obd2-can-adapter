#include <cassert>
#include <cstring>
#include <iostream>

#include "elm_compat.h"

using namespace m5can;

static void expect_reply(ElmCompat& elm, const char* command, const char* reply) {
  const ElmResult r = elm.execute(command);
  assert(r.action == ElmAction::ReplyOnly);
  assert(std::strcmp(r.reply, reply) == 0);
}

int main() {
  ElmCompat elm;

  expect_reply(elm, "ATZ", "M5CAN v0.3 ELM-CAN compatible");
  assert(elm.echo());
  assert(!elm.linefeed());
  assert(!elm.spaces());
  assert(elm.headers());
  assert(elm.protocol() == 7);
  assert(elm.effectiveHeader() == 0x18DB33F1U);

  expect_reply(elm, "ATE0", "OK");
  assert(!elm.echo());
  expect_reply(elm, "ATL1", "OK");
  assert(elm.linefeed());
  expect_reply(elm, "ATS1", "OK");
  assert(elm.spaces());
  expect_reply(elm, "ATH0", "OK");
  assert(!elm.headers());

  expect_reply(elm, "ATAL", "OK");
  expect_reply(elm, "ATCAF1", "OK");
  expect_reply(elm, "ATCFC1", "OK");
  expect_reply(elm, "ATSP7", "OK");
  expect_reply(elm, "ATI", "M5CAN v0.3 ELM-CAN compatible");
  expect_reply(elm, "ATDP", "ISO 15765-4 (CAN 29/500)");
  expect_reply(elm, "ATDPN", "A7");
  expect_reply(elm, "AT@1", "M5CAN-Dial");

  expect_reply(elm, "ATCP18", "OK");
  expect_reply(elm, "ATSHDB33F1", "OK");
  assert(elm.effectiveHeader() == 0x18DB33F1U);
  expect_reply(elm, "ATSHDBEFF1", "OK");
  assert(elm.effectiveHeader() == 0x18DBEFF1U);

  expect_reply(elm, "ATAT2", "OK");
  assert(elm.adaptiveTiming() == 2);
  expect_reply(elm, "ATST0F", "OK");
  assert(elm.timeout4ms() == 0x0F);
  expect_reply(elm, "ATST19", "OK");
  assert(elm.timeout4ms() == 0x19);
  expect_reply(elm, "ATST32", "OK");
  assert(elm.timeout4ms() == 0x32);

  expect_reply(elm, "ATSH18DB33F1", "?");
  expect_reply(elm, "ATCPGG", "?");
  expect_reply(elm, "ATSP6", "?");
  expect_reply(elm, "ATUNKNOWN", "?");

  ElmResult obd = elm.execute("01 0C");
  assert(obd.action == ElmAction::VehicleRead);
  assert(obd.request.can_id == 0x18DBEFF1U);
  assert(obd.request.extended);
  assert(obd.request.length == 2);
  assert(obd.request.data[0] == 0x01 && obd.request.data[1] == 0x0C);
  assert(std::strcmp(obd.reply, "M5CAN TX LOCKED") == 0);

  ElmResult uds = elm.execute("22 20 12");
  assert(uds.action == ElmAction::VehicleRead);
  assert(uds.request.length == 3);
  assert(uds.request.data[0] == 0x22 && uds.request.data[1] == 0x20 &&
         uds.request.data[2] == 0x12);

  expect_reply(elm, "2E201200", "?");
  expect_reply(elm, "1003", "?");
  expect_reply(elm, "2701", "?");
  expect_reply(elm, "3101", "?");

  ElmResult leave = elm.execute("ATM5TEXT");
  assert(leave.action == ElmAction::ExitElmMode);
  assert(std::strcmp(leave.reply, "OK") == 0);

  std::cout << "PASS ELM compatibility command-surface tests\n";
  return 0;
}
