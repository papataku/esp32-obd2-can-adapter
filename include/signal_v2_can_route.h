#pragma once
#include <cstdint>
namespace signal_v2 {
enum class CanIdWidth : uint8_t { Unknown, Standard11, Extended29 };
struct EcuRoute {
  CanIdWidth width=CanIdWidth::Unknown;
  uint32_t request_id=0,response_id=0;
  static EcuRoute fromResponse(uint32_t id,bool extended) {
    EcuRoute r{};
    if(!extended && id>=0x7E8U && id<=0x7EFU){
      r.width=CanIdWidth::Standard11;
      r.request_id=id-8U;
      r.response_id=id;
    }else if(extended && (id&0xFFFFFF00U)==0x18DAF100U){
      r.width=CanIdWidth::Extended29;
      r.request_id=0x18DA00F1U|((id&0xFFU)<<8);
      r.response_id=id;
    }
    return r;
  }
  bool enabledForThisTrial()const {return width==CanIdWidth::Extended29;}
  bool matches(const EcuRoute& other)const{
    return width!=CanIdWidth::Unknown && width==other.width &&
           response_id==other.response_id && request_id==other.request_id;
  }
};
} // namespace signal_v2
