#pragma once
#include <Preferences.h>
#include <cstdlib>
#include <cstring>
#include "evidence_store.h"

namespace m5can {
// Preserve the old BLOB payload ABI and allow read-only legacy NVS retrieval.
// New BLE and full-RAW sidecars never consume the settings/trip NVS partition.
class EvidencePreferences {
 public:
  bool begin(const char* name,bool readonly=false) {
    readonly_=readonly;namespace_=name;
#if defined(ARDUINO_ARCH_ESP32) || defined(M5CAN_EVIDENCE_FLASH_TEST)
    return name && (!std::strcmp(name,"m5can_blelog") || !std::strcmp(name,"m5can_trip"));
#else
    return legacy_.begin(name,readonly);
#endif
  }
  size_t getBytesLength(const char* name) {
#if defined(ARDUINO_ARCH_ESP32) || defined(M5CAN_EVIDENCE_FLASH_TEST)
    EvidenceStore::Type type{};unsigned slot=0;
    if(!decode(name,type,slot))return 0;
    const size_t n=evidenceStore().length(type,slot);
    if(n)return n;
    if(!legacy_.begin(namespace_,true))return 0;
    const size_t old=legacy_.getBytesLength(name);legacy_.end();return old;
#else
    return legacy_.getBytesLength(name);
#endif
  }
  size_t getBytes(const char* name,void* data,size_t bytes) {
#if defined(ARDUINO_ARCH_ESP32) || defined(M5CAN_EVIDENCE_FLASH_TEST)
    EvidenceStore::Type type{};unsigned slot=0;
    if(!decode(name,type,slot))return 0;
    if(evidenceStore().length(type,slot))return evidenceStore().get(type,slot,data,bytes);
    if(!legacy_.begin(namespace_,true))return 0;
    const size_t old=legacy_.getBytes(name,data,bytes);legacy_.end();return old;
#else
    return legacy_.getBytes(name,data,bytes);
#endif
  }
  size_t putBytes(const char* name,const void* data,size_t bytes) {
    if(readonly_)return 0;
#if defined(ARDUINO_ARCH_ESP32) || defined(M5CAN_EVIDENCE_FLASH_TEST)
    EvidenceStore::Type type{};unsigned slot=0;
    return decode(name,type,slot) && evidenceStore().put(type,slot,data,bytes)?bytes:0;
#else
    return legacy_.putBytes(name,data,bytes);
#endif
  }
  void end(){legacy_.end();namespace_=nullptr;}
 private:
  bool decode(const char* name,EvidenceStore::Type& type,unsigned& slot) const {
    if(!name || !namespace_)return false;
    const char* suffix=nullptr;
    if(!std::strcmp(namespace_,"m5can_blelog") && !std::strncmp(name,"ble",3)){
      type=EvidenceStore::Ble;suffix=name+3;
    } else if(!std::strcmp(namespace_,"m5can_trip")){
      if(!std::strncmp(name,"probe",5)){type=EvidenceStore::Probe;suffix=name+5;}
      else if(!std::strncmp(name,"fail",4)){type=EvidenceStore::Failure;suffix=name+4;}
    }
    if(!suffix || !*suffix)return false;
    for(const char* p=suffix;*p;++p)if(*p<'0' || *p>'9')return false;
    slot=static_cast<unsigned>(std::strtoul(suffix,nullptr,10));
    return slot<(type==EvidenceStore::Ble?4U:8U);
  }
  Preferences legacy_;
  const char* namespace_=nullptr;
  bool readonly_=false;
};
} // namespace m5can
