#include "evidence_store.h"
#include <cstring>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_partition.h>
#endif

namespace m5can {
namespace {
constexpr uint32_t kOwner=0x4D354F57U,kRecord=0x4D354552U,kCommit=0x434F4D54U;
uint32_t crcBytes(uint32_t crc,const void* p,size_t n) {
  const auto* b=static_cast<const uint8_t*>(p);
  while(n--) {
    crc^=*b++;
    for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xEDB88320U:0);
  }
  return crc;
}
#if defined(ARDUINO_ARCH_ESP32)
class PartitionFlash final:public EvidenceFlash {
 public:
  const esp_partition_t* partition=nullptr;
  uint32_t size() const override {return partition?partition->size:0;}
  bool read(uint32_t o,void* p,size_t n) override {
    return partition && esp_partition_read(partition,o,p,n)==ESP_OK;
  }
  bool write(uint32_t o,const void* p,size_t n) override {
    return partition && esp_partition_write(partition,o,p,n)==ESP_OK;
  }
  bool erase(uint32_t o) override {
    return partition && esp_partition_erase_range(partition,o,EvidenceStore::kSector)==ESP_OK;
  }
};
#endif
}
EvidenceStore& evidenceStore(){static EvidenceStore store;return store;}
bool initializeEvidenceStore() {
#if defined(ARDUINO_ARCH_ESP32) && defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  static PartitionFlash flash;
  flash.partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"spiffs");
  // Never mount/format a filesystem, change a table, use encrypted partitions,
  // or claim an occupied foreign partition. Blank ownership is checked below.
  if(!flash.partition || flash.partition->encrypted)return false;
  return evidenceStore().begin(flash,true);
#else
  return false;
#endif
}
int EvidenceStore::key(Type type,unsigned slot) {
  if(type==Probe && slot<8)return slot;
  if(type==Failure && slot<8)return 8+slot;
  if(type==Ble && slot<4)return 16+slot;
  return -1;
}
uint32_t EvidenceStore::checksum(const Header& h,const void* data,size_t bytes) {
  uint32_t crc=crcBytes(UINT32_MAX,&h,offsetof(Header,crc));
  return ~crcBytes(crc,data,bytes);
}
bool EvidenceStore::owner(const Header& h) const {
  return h.magic==kOwner && h.version==1 && h.type==0 &&
      h.sequence==0 && h.slot==0 && h.bytes==flash_->size() &&
      h.commit==kCommit && h.crc==checksum(h,nullptr,0);
}
bool EvidenceStore::valid(uint32_t offset,Header& h) {
  if(!flash_->read(offset,&h,sizeof(h))){++status_.errors;return false;}
  if(h.magic==UINT32_MAX)return false;
  if(h.commit!=kCommit){++status_.uncommitted;return false;}
  if(h.magic!=kRecord || h.version!=1 || !h.sequence ||
      key(static_cast<Type>(h.type),h.slot)<0 || !h.bytes || h.bytes>kMaxBlob) {
    ++status_.corrupt;return false;
  }
  if(!flash_->read(offset+sizeof(h),scratch_,h.bytes)){
    ++status_.errors;return false;
  }
  if(h.crc!=checksum(h,scratch_,h.bytes)){++status_.corrupt;return false;}
  return true;
}
bool EvidenceStore::begin(EvidenceFlash& flash,bool initialize_blank) {
  flash_=&flash;status_=Status{};cursor_=2;
  for(auto& i:index_)i=Index{};
  const uint32_t size=flash.size();
  if(size%kSector || size<(kKeys+4)*kSector){++status_.errors;return false;}
  status_.sectors=size/kSector;
  Header a{},b{};
  if(!flash.read(0,&a,sizeof(a)) || !flash.read(kSector,&b,sizeof(b))){
    ++status_.errors;return false;
  }
  if(!owner(a) && !owner(b)) {
    // Full-partition erased check precedes ANY mutation. A SPIFFS/LittleFS
    // image, partially initialized unknown owner, or foreign bytes fail shut.
    for(uint32_t offset=0;offset<size;) {
      const size_t n=(size-offset<kMaxBlob)?size-offset:kMaxBlob;
      if(!flash.read(offset,scratch_,n)){++status_.errors;return false;}
      for(size_t i=0;i<n;++i)if(scratch_[i]!=0xFF){
        status_.foreign=true;return false;
      }
      offset+=n;
    }
    if(!initialize_blank)return false;
    Header h{};h.magic=kOwner;h.bytes=size;h.crc=checksum(h,nullptr,0);
    // First committed owner is sufficient after a cut during the second.
    for(unsigned copy=0;copy<2;++copy) {
      const uint32_t offset=copy*kSector;
      if(!flash.write(offset,&h,sizeof(h)) ||
         !flash.write(offset+offsetof(Header,commit),&kCommit,sizeof(kCommit))){
        ++status_.errors;return false;
      }
    }
  }
  for(uint32_t sector=2;sector<status_.sectors;++sector) {
    Header h{};
    if(!valid(sector*kSector,h))continue;
    const int k=key(static_cast<Type>(h.type),h.slot);
    if(h.sequence>index_[k].sequence) {
      index_[k].offset=sector*kSector;index_[k].bytes=h.bytes;
      index_[k].sequence=h.sequence;
    }
    if(h.sequence>status_.last_sequence){
      status_.last_sequence=h.sequence;cursor_=sector+1;
    }
  }
  if(cursor_>=status_.sectors)cursor_=2;
  status_.ready=true;return true;
}
bool EvidenceStore::protectedSector(uint32_t offset) const {
  for(const auto& i:index_)if(i.offset==offset)return true;
  return false;
}
bool EvidenceStore::put(Type type,unsigned slot,const void* data,size_t bytes) {
  const int k=key(type,slot);
  if(!status_.ready || k<0 || !data || !bytes || bytes>kMaxBlob ||
      status_.last_sequence==UINT64_MAX){++status_.errors;return false;}
  uint32_t offset=UINT32_MAX;
  for(uint32_t tries=0;tries<status_.sectors-2;++tries){
    const uint32_t candidate=cursor_*kSector;
    if(++cursor_>=status_.sectors)cursor_=2;
    // Preserve every key's latest valid record until the NEW record commits.
    if(!protectedSector(candidate)){offset=candidate;break;}
  }
  if(offset==UINT32_MAX){++status_.errors;return false;}
  Header h{};h.magic=kRecord;h.type=type;h.slot=slot;
  h.sequence=++status_.last_sequence;h.bytes=bytes;h.crc=checksum(h,data,bytes);
  if(!flash_->erase(offset) || !flash_->write(offset,&h,sizeof(h)) ||
      !flash_->write(offset+sizeof(h),data,bytes) ||
      !flash_->write(offset+offsetof(Header,commit),&kCommit,sizeof(kCommit))){
    ++status_.errors;return false;
  }
  Header verified{};
  if(!valid(offset,verified) || verified.sequence!=h.sequence){
    ++status_.errors;return false;
  }
  index_[k].offset=offset;index_[k].bytes=h.bytes;index_[k].sequence=h.sequence;
  ++status_.commits;return true;
}
size_t EvidenceStore::length(Type type,unsigned slot) const {
  const int k=key(type,slot);
  return status_.ready && k>=0?index_[k].bytes:0;
}
size_t EvidenceStore::get(Type type,unsigned slot,void* data,size_t bytes) {
  const int k=key(type,slot);
  if(!status_.ready || k<0 || !data || index_[k].bytes!=bytes ||
      index_[k].offset==UINT32_MAX)return 0;
  Header h{};
  if(!valid(index_[k].offset,h) || h.sequence!=index_[k].sequence)return 0;
  std::memcpy(data,scratch_,bytes);return bytes;
}
} // namespace m5can
