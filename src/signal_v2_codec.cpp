#include "signal_v2_codec.h"
#include <algorithm>
#include <cstring>
namespace signal_v2 {
uint32_t crc(const uint8_t* p,size_t n,bool pdu){uint32_t c=~0U;for(size_t i=0;i<n;++i){c^=(pdu&&i>=22&&i<26)?0:p[i];for(int j=0;j<8;++j)c=(c>>1)^((0U-(c&1))&0xedb88320U);}return ~c;}
static bool records(const uint8_t* p,size_t n,bool snap){
 size_t off=snap?16:2;if(n<off)return false;
 unsigned count=snap?p[12]:p[0];
 if(snap){if(p[13]||p[14]||p[15])return false;}else if(p[1])return false;
 for(unsigned i=0;i<count;++i){if(n-off<44)return false;auto r=p+off;size_t len=u16(r);if(len<44||len>n-off||r[2]!=1||r[7]>10||r[43]||len!=size_t(44+r[42]))return false;off+=len;}
 return off==n;
}
Error decode(const uint8_t* b,size_t n,Packet& out){
 if(n<Header||n>MaxPdu||size_t(u16(b+20))+Header!=n)return Error::Length;
 if(b[0]!=0x4d||b[1]!=0x35||b[2]!=2||b[3]||b[5]||b[6]!=26||b[7])return Error::Header;
 auto t=b[4];if(t!=1&&t!=0x20&&t!=0x21&&t!=0x22&&t!=0x80&&t!=0x81)return Error::Header;
 if(u32(b+22)!=crc(b,n,true))return Error::Crc;
 const uint8_t* p=b+Header;size_t z=n-Header;
 if(t==1&&(z<12||p[9]||size_t(u16(p+10))+12!=z||p[8]<1||p[8]>11))return Error::Payload;
 if(t==0x80&&(z<16||u16(p+6)||u16(p+14)||size_t(u16(p+12))+16!=z||p[5]>11))return Error::Payload;
 if(t==0x20&&!records(p,z,false))return Error::Payload;
 if(t==0x21&&!records(p,z,true))return Error::Payload;
 if(t==0x22&&z!=12)return Error::Payload;
 std::copy(b,b+n,out.bytes.begin());out.size=n;return Error::None;
}
Packet encode(uint8_t type,uint32_t boot,uint32_t session,uint32_t seq,const uint8_t* p,size_t n){
 Packet r;if(n>MaxPdu-Header)return r;r.size=Header+n;auto b=r.bytes.data();b[0]=0x4d;b[1]=0x35;b[2]=2;b[4]=type;b[6]=26;w32(b+8,boot);w32(b+12,session);w32(b+16,seq);w16(b+20,n);if(n)std::memcpy(b+Header,p,n);w32(b+22,crc(b,r.size,true));return r;
}
Error Reassembler::feed(const uint8_t* b,size_t n,uint8_t lane,uint32_t now,Packet& out){
 if(active_&&uint32_t(now-started_)>=2000)reset();
 if(n<17||n>260||b[0]!=0xd7||b[1]!=0x52||b[2]!=2||b[3]!=lane||(b[4]&~3)||b[5]||b[15]||n!=size_t(16+b[14])||!b[14]){reset();return Error::Fragment;}
 size_t total=u16(b+10),off=u16(b+12),len=b[14];
 if(total<Header||total>MaxPdu||off>total||len>total-off){reset();return Error::Fragment;}
 if(b[4]&1){if(active_||off){reset();return Error::Fragment;}active_=true;started_=now;transfer_=u32(b+6);total_=total;offset_=0;}
 if(!active_||transfer_!=u32(b+6)||total_!=total||off!=offset_||bool(b[4]&2)!=(off+len==total)){reset();return Error::Fragment;}
 std::memcpy(p_.bytes.data()+off,b+16,len);offset_+=len;
 if(!(b[4]&2))return Error::Pending;
 reset();return decode(p_.bytes.data(),total,out);
}
size_t fragment(const Packet& p,uint8_t lane,uint32_t id,size_t off,size_t mtu,uint8_t* out){
 if(mtu<23||off>=p.size)return 0;size_t len=std::min({mtu-19,size_t(244),p.size-off});
 std::memset(out,0,16);out[0]=0xd7;out[1]=0x52;out[2]=2;out[3]=lane;out[4]=(off==0?1:0)|(off+len==p.size?2:0);w32(out+6,id);w16(out+10,p.size);w16(out+12,off);out[14]=len;std::memcpy(out+16,p.bytes.data()+off,len);return 16+len;
}
}
