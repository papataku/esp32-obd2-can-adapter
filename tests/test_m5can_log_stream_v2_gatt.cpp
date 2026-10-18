#include "../src/m5can_log_stream_v2_gatt.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace m5can_log_stream_v2;
int main() {
  uint8_t bytes[244]{};
  StartRequest start{};
  start.session=0x3A2B1C4D;start.blocks=42;
  assert(encodeStart(start,bytes,sizeof(bytes))==20);
  Control parsed{};
  assert(decodeControl(bytes,20,parsed));
  assert(parsed.kind==ControlKind::Start);
  assert(parsed.start.session==start.session && parsed.start.blocks==42);
  assert(!decodeControl(bytes,19,parsed));
  bytes[8]^=1;assert(!decodeControl(bytes,20,parsed));bytes[8]^=1;
  bytes[14]=1;assert(!decodeControl(bytes,20,parsed));bytes[14]=0;
  bytes[5]=5;assert(!decodeControl(bytes,20,parsed));bytes[5]=1;
  assert(!decodeControl(bytes,244,parsed));
  StartRequest invalid{};invalid.blocks=2;
  assert(encodeStart(invalid,bytes,sizeof(bytes))==0);
  invalid.session=1;invalid.blocks=4097;
  assert(encodeStart(invalid,bytes,sizeof(bytes))==0);
  Frame ack{};
  ack.kind=Kind::Ack;ack.session=start.session;
  ack.sequence=12;ack.block=2;ack.offset=480;
  assert(encode(ack,bytes,sizeof(bytes))==28);
  assert(decodeControl(bytes,28,parsed));
  assert(parsed.kind==ControlKind::Ack && parsed.ack.sequence==12);
  bytes[19]^=0x80;
  assert(!decodeControl(bytes,28,parsed));
  Frame data{};uint8_t raw[1]{7};
  data.kind=Kind::Data;data.session=start.session;data.sequence=12;
  data.block=0;data.offset=0;data.total=1;data.length=1;data.data=raw;
  assert(encode(data,bytes,sizeof(bytes))==29);
  assert(!decodeControl(bytes,29,parsed)); // data forbidden on CTRL
  std::cout<<"M5CAN V2 dedicated GATT control integrity test PASS\n";
}
