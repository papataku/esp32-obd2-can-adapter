#if defined(M5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB)
#include "signal_v2_can_owner.h"
#include "twai_hil.h"
namespace {
m5can::CanMonitor monitor;
signal_v2::CanOwner owner;
uint32_t epoch=1;
bool armed=false;
void command(const char* line) {
  if(hil::usbCommand(line))return;
  if(!std::strcmp(line,"P5 STOP") || !std::strcmp(line,"P5 PROFILE CHANGE")) {
    armed=false;owner.revokeAuthorization(++epoch);owner.invalidate();
  } else if(!std::strcmp(line,"P5 ARM")) {
    const bool ok=!armed && !owner.busy() && owner.armDiscovery11(millis(),epoch+1,false);
    if(ok){++epoch;armed=true;}
    Serial.printf("#P5,armed=%u,LIVE=0,epoch=%lu\n",ok?1U:0U,(unsigned long)epoch);
  }
}
}
void setup(){Serial.begin(115200);monitor.begin();owner.attach(&monitor);Serial.println("#P5,CANLESS_LAB=1,LIVE=0,armed=0");}
void loop(){
  static char line[129]{};static unsigned used=0;static bool overflow=false;
  for(unsigned n=0;n<128 && Serial.available();++n){
    const char c=char(Serial.read());
    if(c=='\n'){if(!overflow){line[used]=0;command(line);}used=0;overflow=false;}
    else if(c!='\r'){if(used<128)line[used++]=c;else overflow=true;}
  }
  owner.tick(millis(),{},armed,epoch);
  if(owner.stopped())armed=false;
  hil::poll();delay(1);
}
#endif
