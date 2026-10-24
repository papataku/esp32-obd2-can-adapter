#pragma once
// Issue #54 extension: a bounded, non-unique ECU-set fingerprint.
// This is a hint, NEVER a vehicle ID or a CAN transmit authorization.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "ecu_physical_selector.h"

namespace m5can {
struct EcuVehicleHint {
  uint32_t magic=0x45435548U; // ECUH
  uint16_t version=1;
  uint8_t count=0;
  uint8_t reserved=0;
  uint32_t source=0;
  uint32_t responders[kDiagnosticMaxResponses]{};
  uint32_t crc=0;
};
class EcuHintCodec {
 public:
  static uint32_t checksum(const EcuVehicleHint& item) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(&item);
    uint32_t crc=0xffffffffU;
    for(size_t n=0;n<offsetof(EcuVehicleHint,crc);++n) {
      crc^=bytes[n];
      for(unsigned j=0;j<8;++j)
        crc=(crc>>1)^((crc&1U)?0xedb88320U:0U);
    }
    return crc^0xffffffffU;
  }
  static bool valid(const EcuVehicleHint& h) {
    if(h.magic!=0x45435548U || h.version!=1 || h.reserved!=0 ||
       h.count==0 || h.count>kDiagnosticMaxResponses ||
       (h.source&0x1fffff00U)!=0x18daf100U || h.crc!=checksum(h))
      return false;
    bool found=false;
    for(uint8_t i=0;i<h.count;++i) {
      const uint32_t id=h.responders[i];
      if((id&0x1fffff00U)!=0x18daf100U ||
         (i && h.responders[i-1]>=id))return false;
      if(id==h.source)found=true;
    }
    for(uint8_t i=h.count;i<kDiagnosticMaxResponses;++i)
      if(h.responders[i])return false;
    return found;
  }
  // All responders must be complete, unique, and have their exact original
  // CAN Single Frame. No partial/negative/virtual bytes form a fingerprint.
  static bool fromFunctional(const DiagnosticResult& r,uint32_t selected,
                             EcuVehicleHint& out) {
    out=EcuVehicleHint{};
    if(r.status!=DiagnosticStatus::Ok || !r.response_count ||
       r.response_count>kDiagnosticMaxResponses)return false;
    out.source=selected;
    out.count=r.response_count;
    bool contains=false;
    for(uint8_t i=0;i<r.response_count;++i) {
      const auto& m=r.responses[i];
      uint16_t rpm=0;uint8_t speed=0;
      if(!EcuPhysicalSelector::parsePair(m,rpm,speed))return false;
      bool backed=false;
      for(uint8_t j=0;j<r.raw_frame_count && j<kDiagnosticMaxRawFrames;++j){
        const auto& raw=r.raw_frames[j];
        if(raw.can_id==m.can_id && raw.dlc>=7 &&
           raw.data[0]==6 && std::memcmp(raw.data+1,m.data,6)==0)
          backed=true;
      }
      if(!backed)return false;
      if(m.can_id==selected)contains=true;
      uint8_t index=i;
      while(index && out.responders[index-1]>m.can_id) {
        out.responders[index]=out.responders[index-1];--index;
      }
      out.responders[index]=m.can_id;
    }
    if(!contains)return false;
    out.crc=checksum(out);
    return valid(out);
  }
  static bool matches(const EcuVehicleHint& hint,const DiagnosticResult& now) {
    if(!valid(hint))return false;
    EcuVehicleHint fresh{};
    if(!fromFunctional(now,hint.source,fresh))return false;
    return fresh.count==hint.count &&
           std::memcmp(fresh.responders,hint.responders,
                       sizeof(hint.responders))==0;
  }
};
} // namespace m5can
