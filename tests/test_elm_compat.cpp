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

  expect_reply(elm, "ATZ", "M5CAN v0.4 ELM-CAN compatible");
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
  expect_reply(elm, "ATI", "M5CAN v0.4 ELM-CAN compatible");
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

  {
    const ElmResult caps = elm.execute("ATM5CAP");
    assert(caps.action == ElmAction::ReplyOnly);
    assert(std::strstr(caps.reply, "M5CAN-CAPS PROTO=1.1 ") != nullptr);
    assert(std::strstr(caps.reply, "FW=") != nullptr);
    assert(std::strstr(caps.reply, "BATCH=16 ") != nullptr);
    assert(std::strstr(caps.reply, "OPS=OBD01,UDS22 ") != nullptr);
    assert(std::strstr(caps.reply, "LEASE=IMPLICIT") != nullptr);
  }
  {
    const ElmResult one = elm.execute("ATM5B01:2012");
    assert(one.action == ElmAction::BatchRead && one.batch_count == 1);
    assert(one.batch[0].can_id == 0x18DA01F1U);
    assert(one.batch[0].data[0] == 0x22);
    assert(one.batch[0].data[1] == 0x20);
    assert(one.batch[0].data[2] == 0x12);
    const ElmResult obds = elm.execute("ATM5B00:010C,010D,0105,015B,019A");
    assert(obds.action == ElmAction::BatchRead && obds.batch_count == 5);
    assert(obds.batch[0].can_id == 0x18DB33F1U);
    assert(obds.batch[4].data[0] == 0x01);
    assert(obds.batch[4].data[1] == 0x9A);
  }
  {
    const ElmResult uds = elm.execute("ATM5B01:2012,E480,E481,E600,E602");
    assert(uds.action == ElmAction::BatchRead && uds.batch_count == 5);
    expect_reply(elm, "ATM5B00:010C,010D,", "?");
    expect_reply(elm, "ATM5B00:010C,2E12", "?");
    expect_reply(elm, "ATM5B02:2012", "?");
    expect_reply(elm, "ATM5B01:2012,AB", "?");
    expect_reply(elm, "ATM5B01:2012;", "?");
    expect_reply(elm, "ATM5B01:", "?");
  }

  assert(elm.execute("ATM5TX1").action == ElmAction::LeaseAcquire);
  assert(elm.execute("ATM5TX0").action == ElmAction::LeaseRevoke);
  assert(elm.execute("ATM5STAT").action == ElmAction::LeaseStatus);

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
