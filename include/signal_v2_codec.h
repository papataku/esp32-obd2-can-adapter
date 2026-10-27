#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace signal_v2 {
constexpr size_t MaxPdu=512, Header=26;
constexpr const char* Service="f9c17d00-2a3e-4e18-89a2-d7c1b539e000";
constexpr const char* Control="f9c17d01-2a3e-4e18-89a2-d7c1b539e000";
constexpr const char* Data="f9c17d02-2a3e-4e18-89a2-d7c1b539e000";
constexpr const char* Status="f9c17d03-2a3e-4e18-89a2-d7c1b539e000";
inline uint16_t u16(const uint8_t* p){return p[0]|uint16_t(p[1])<<8;}
inline uint32_t u32(const uint8_t* p){return u16(p)|uint32_t(u16(p+2))<<16;}
inline void w16(uint8_t* p,uint16_t n){p[0]=n;p[1]=n>>8;}
inline void w32(uint8_t* p,uint32_t n){w16(p,n);w16(p+2,n>>16);}
inline void w64(uint8_t* p,uint64_t n){w32(p,n);w32(p+4,n>>32);}
struct Packet {std::array<uint8_t,MaxPdu> bytes{}; size_t size=0; uint8_t* payload(){return bytes.data()+Header;} const uint8_t* payload()const{return bytes.data()+Header;}};
enum class Error {None, Length, Header, Crc, Payload, Fragment, Pending};
uint32_t crc(const uint8_t*,size_t,bool pdu=false);
Error decode(const uint8_t*,size_t,Packet&);
Packet encode(uint8_t type,uint32_t boot,uint32_t session,uint32_t seq,const uint8_t*,size_t);
// One bounded transfer per lane/connection. Never accept overlap or restart an
// in-flight transfer; a timeout resets the lane before a new FIRST is admitted.
class Reassembler {
 Packet p_{}; uint32_t transfer_=0,started_=0; size_t offset_=0,total_=0; bool active_=false;
 public:
 void reset(){active_=false;offset_=total_=0;}
 Error feed(const uint8_t*,size_t,uint8_t lane,uint32_t now,Packet&);
};
size_t fragment(const Packet&,uint8_t lane,uint32_t transfer,size_t offset,size_t mtu,uint8_t* out);
}
