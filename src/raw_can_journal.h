#pragma once
// Append-only, owner-protected CAN RAW journal for an explicitly migrated
// custom 'canlog' flash partition. No mount, erase, formatting or NVS writes.
// An unknown/previous OTA partition MUST fail closed; never assume blankness.
#include "raw_can_codec.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace m5can {

class RawCanFlash {
 public:
  virtual ~RawCanFlash() = default;
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset,void* data,size_t bytes) = 0;
  virtual bool write(uint32_t offset,const void* data,size_t bytes) = 0;
};

class RawCanJournal {
 public:
  static constexpr uint32_t kSector=4096;
  static constexpr uint32_t kHeader=32;
  static constexpr uint32_t kCommit=0x434F4D54;  // 'COMT', final write
  static constexpr uint8_t kSchema=1;
  enum RecordType : uint8_t { Data=1, End=2 };
  struct Status {
    bool ready=false, foreign=false, full=false, corrupt=false;
    uint32_t sectors=0, blocks=0, bytes=0, session=0, errors=0;
    uint32_t last_sequence=0, last_dropped=0;
    bool completed=false;
  };

  const Status& status() const { return status_; }

  bool begin(RawCanFlash& flash) {
    flash_=&flash;status_=Status{};
    const uint32_t size=flash.size();
    if(size%kSector || size<4*kSector) {
      ++status_.errors;return false;
    }
    status_.sectors=size/kSector;
    uint8_t owners[2][20]{};
    if(!flash.read(0,owners[0],20) ||
       !flash.read(kSector,owners[1],20)) {
      ++status_.errors;return false;
    }
    const bool valid0=validOwner(owners[0],size);
    const bool valid1=validOwner(owners[1],size);
    if(!valid0 && !valid1) {
      // MUST scan the ENTIRE region; a blank initial sector does not prove
      // the former OTA image was erased by an approved migration.
      for(uint32_t sector=0;sector<status_.sectors;++sector) {
        if(!blank(sector)) {
          status_.foreign=true;return false;
        }
      }
      if(!putOwner(0,size) || !putOwner(kSector,size)) {
        ++status_.errors;return false;
      }
    }
    // Scan strictly sequential append-only blocks. Never skip a corrupt
    // sector and overwrite the next one: preserve forensic evidence.
    for(uint32_t sector=2;sector<status_.sectors;++sector) {
      uint8_t header[kHeader]{};
      const uint32_t offset=sector*kSector;
      if(!flash.read(offset,header,sizeof(header))) {
        ++status_.errors;return false;
      }
      if(erased(header,sizeof(header))) {
        // Also reject partially written sectors with an erased prefix.
        if(!blank(sector)) {
          status_.corrupt=true;return false;
        }
        break;
      }
      if(!validRecord(offset,header)) {
        status_.corrupt=true;return false;
      }
      if(status_.blocks && get32(header+12)!=status_.blocks) {
        status_.corrupt=true;return false;
      }
      if(status_.session && get32(header+8)!=status_.session) {
        status_.corrupt=true;return false;
      }
      status_.session=get32(header+8);
      status_.last_dropped=get32(header+28);
      status_.bytes+=get16(header+6);
      status_.completed=(header[5]==End);
      ++status_.blocks;
    }
    status_.full=status_.blocks>=status_.sectors-2;
    status_.ready=true;
    return true;
  }

  // Called only by the Arduino journal owner while holding CAN-idle
  // reservation; never from TWAI owner, BLE callbacks or display ISRs.
  // v1 intentionally holds one protected session. An explicit, verified
  // meter-SD receipt and separate reclaim protocol are prerequisites
  // before a second recording may overwrite the existing session.
  bool append(uint32_t session,RecordType type,const uint8_t* bytes,
              uint16_t length,uint32_t dropped) {
    if(!status_.ready || !session || status_.full ||
       (status_.session && status_.session!=session) ||
       status_.completed || (type!=Data && type!=End) ||
       length>RawCanBlockCodec::kMaxBytes ||
       (type==Data && (!bytes || length<RawCanBlockCodec::kHeader)) ||
       (type==End && length!=0)) {
      ++status_.errors;return false;
    }
    const uint32_t sector=status_.blocks+2;
    if(sector>=status_.sectors || !blank(sector)) {
      status_.foreign=true; ++status_.errors; return false;
    }
    uint8_t header[kHeader];
    std::memset(header,0xFF,sizeof(header));
    header[0]='C';header[1]='A';header[2]='N';header[3]='B';
    header[4]=kSchema;header[5]=type;
    put16(header+6,length);
    put32(header+8,session);
    put32(header+12,status_.blocks);
    put32(header+16, type==Data ? get16(bytes+6) : 0);
    put32(header+28,dropped);
    uint32_t crc=checksum(header,bytes,length);
    put32(header+20,crc);
    const uint32_t offset=sector*kSector;
    // Commit-last, no erase: old or future foreign data is never erased.
    if(!flash_->write(offset,header,24) ||
       !flash_->write(offset+28,header+28,4) ||
       (length && !flash_->write(offset+kHeader,bytes,length)) ||
       !flash_->write(offset+24,&kCommit,sizeof(kCommit))) {
      ++status_.errors;return false;
    }
    uint8_t verified[kHeader]{};
    if(!flash_->read(offset,verified,sizeof(verified)) ||
       !validRecord(offset,verified)) {
      ++status_.errors;return false;
    }
    status_.session=session;
    status_.last_dropped=dropped;
    status_.bytes+=length;
    status_.completed=(type==End);
    ++status_.blocks;
    status_.full=status_.blocks>=status_.sectors-2;
    return true;
  }

  // Nonmutating, integrity-checked random access for future BLE LOG_READ.
  bool readBlock(uint32_t index,uint8_t* data,size_t cap,
                 uint16_t& length,RecordType& type) {
    length=0;
    if(!status_.ready || !data || index>=status_.blocks)return false;
    const uint32_t offset=(index+2)*kSector;
    uint8_t header[kHeader]{};
    if(!flash_->read(offset,header,kHeader) ||
       !validRecord(offset,header))return false;
    const uint16_t n=get16(header+6);
    if(n>cap || (n && !flash_->read(offset+kHeader,data,n)))return false;
    type=static_cast<RecordType>(header[5]);
    length=n;return true;
  }

 private:
  static void put16(uint8_t* p,uint16_t x) {
    p[0]=static_cast<uint8_t>(x);p[1]=static_cast<uint8_t>(x>>8);
  }
  static void put32(uint8_t* p,uint32_t x) {
    for(unsigned i=0;i<4;++i)p[i]=static_cast<uint8_t>(x>>(8*i));
  }
  static uint16_t get16(const uint8_t* p) {
    return uint16_t(p[0]) | uint16_t(p[1])<<8;
  }
  static uint32_t get32(const uint8_t* p) {
    uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);
    return v;
  }
  static bool erased(const uint8_t* p,size_t n) {
    for(size_t i=0;i<n;++i)if(p[i]!=0xFFU)return false;
    return true;
  }
  static uint32_t updateCrc(uint32_t crc,const uint8_t* p,size_t n) {
    while(n--) {
      crc^=*p++;
      for(unsigned i=0;i<8;++i)
        crc=(crc>>1)^((crc&1U)?0xEDB88320U:0);
    }
    return crc;
  }
  static uint32_t checksum(const uint8_t* header,const uint8_t* bytes,
                           size_t length) {
    uint32_t crc=0xFFFFFFFFU;
    crc=updateCrc(crc,header,20);
    crc=updateCrc(crc,header+28,4);
    if(length)crc=updateCrc(crc,bytes,length);
    return ~crc;
  }
  bool blank(uint32_t sector) {
    uint8_t bytes[256];
    for(uint32_t position=0;position<kSector;position+=sizeof(bytes)) {
      if(!flash_->read(sector*kSector+position,bytes,sizeof(bytes)))
        return false;
      if(!erased(bytes,sizeof(bytes)))return false;
    }
    return true;
  }
  static bool validOwner(const uint8_t* data,uint32_t size) {
    if(std::memcmp(data,"CANO",4) || data[4]!=kSchema ||
       get32(data+8)!=size || get32(data+16)!=kCommit)return false;
    uint32_t crc=~updateCrc(0xFFFFFFFFU,data,12);
    return crc==get32(data+12);
  }
  bool putOwner(uint32_t offset,uint32_t size) {
    uint8_t header[20];
    std::memset(header,0xFF,sizeof(header));
    std::memcpy(header,"CANO",4);
    header[4]=kSchema;
    put32(header+8,size);
    put32(header+12,~updateCrc(0xFFFFFFFFU,header,12));
    put32(header+16,kCommit);
    if(!flash_->write(offset,header,16) ||
       !flash_->write(offset+16,header+16,4))return false;
    uint8_t verify[20]{};
    return flash_->read(offset,verify,20) && validOwner(verify,size);
  }
  bool validRecord(uint32_t offset,const uint8_t* header) {
    if(std::memcmp(header,"CANB",4) || header[4]!=kSchema ||
       (header[5]!=Data && header[5]!=End) ||
       get32(header+24)!=kCommit || get32(header+8)==0)return false;
    const uint16_t length=get16(header+6);
    if(length>RawCanBlockCodec::kMaxBytes ||
       (header[5]==End && length!=0) ||
       (header[5]==Data && length<RawCanBlockCodec::kHeader))
      return false;
    uint32_t crc=updateCrc(0xFFFFFFFFU,header,20);
    crc=updateCrc(crc,header+28,4);
    uint8_t scratch[256];
    for(uint32_t done=0;done<length;) {
      const size_t chunk=(length-done>sizeof(scratch))?
          sizeof(scratch):length-done;
      if(!flash_->read(offset+kHeader+done,scratch,chunk))return false;
      crc=updateCrc(crc,scratch,chunk);
      done+=chunk;
    }
    return (~crc)==get32(header+20);
  }

  RawCanFlash* flash_=nullptr;
  Status status_{};
};

} // namespace m5can
