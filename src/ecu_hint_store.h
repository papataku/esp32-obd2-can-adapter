#pragma once
// Only the Arduino/UI loop may access Preferences/NVS. The BLE worker
// exchanges bounded snapshots through a guarded RAM mailbox.
#include <Arduino.h>
#include <Preferences.h>
#include "can_monitor.h"
#include "ecu_vehicle_hint.h"

namespace m5can {
class EcuHintStore {
 public:
  void loadBeforeBleStarts() {
    EcuVehicleHint item{};
    Preferences prefs;
    bool read=false;
    if(prefs.begin("m5can_ecu",true)) {
      read=prefs.getBytes("hint",&item,sizeof(item))==sizeof(item);
      prefs.end();
    }
    boot_valid_=read && EcuHintCodec::valid(item);
    if(boot_valid_)boot_=item;
  }
  bool bootHint(EcuVehicleHint& out) const {
    if(!boot_valid_)return false;
    out=boot_;
    return true;
  }
  void invalidateBootHint() { boot_valid_=false; }
  void stage(const EcuVehicleHint& hint) {
    if(!EcuHintCodec::valid(hint))return;
    portENTER_CRITICAL(&mux_);
    pending_=hint;
    ++pending_generation_;
    dirty_=true;
    portEXIT_CRITICAL(&mux_);
  }
  // Called only from Arduino loop, never BLE callback or CAN owner.
  bool poll(CanMonitor& can) {
    if(millis()<1000 || (last_attempt_ms_ && millis()-last_attempt_ms_<5000))
      return false;
    EcuVehicleHint item{};
    uint32_t generation=0;
    portENTER_CRITICAL(&mux_);
    const bool queued=dirty_;
    if(queued){item=pending_;generation=pending_generation_;}
    portEXIT_CRITICAL(&mux_);
    if(!queued || !can.tryBeginLogCheckpoint())return false;
    last_attempt_ms_=millis();
    Preferences prefs;
    bool stored=false;
    if(prefs.begin("m5can_ecu",false)) {
      stored=prefs.putBytes("hint",&item,sizeof(item))==sizeof(item);
      prefs.end();
    }
    EcuVehicleHint verified{};
    Preferences check;
    bool read=false;
    if(check.begin("m5can_ecu",true)) {
      read=check.getBytes("hint",&verified,sizeof(verified))==sizeof(verified);
      check.end();
    }
    const bool ok=stored && read && EcuHintCodec::valid(verified) &&
      std::memcmp(&verified,&item,sizeof(item))==0;
    can.endLogCheckpoint();
    if(ok) {
      portENTER_CRITICAL(&mux_);
      if(pending_generation_==generation)dirty_=false;
      portEXIT_CRITICAL(&mux_);
      Serial.printf("#ECU_HINT,persisted=1,source=%08lX,count=%u\n",
                    static_cast<unsigned long>(item.source),unsigned(item.count));
    } else Serial.println("#ECU_HINT,persisted=0,write_or_crc_error");
    return ok;
  }
 private:
  mutable portMUX_TYPE mux_=portMUX_INITIALIZER_UNLOCKED;
  EcuVehicleHint boot_{},pending_{};
  bool boot_valid_=false,dirty_=false;
  uint32_t pending_generation_=0,last_attempt_ms_=0;
};
} // namespace m5can
