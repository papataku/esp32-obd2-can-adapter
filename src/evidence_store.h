#pragma once
#include <cstddef>
#include <cstdint>

namespace m5can {
// Only the Arduino owner may use this store, while holding the CAN-idle
// checkpoint reservation. No callback, continuous bus dump, or filesystem.
class EvidenceFlash {
 public:
  virtual ~EvidenceFlash()=default;
  virtual uint32_t size() const=0;
  virtual bool read(uint32_t offset,void* data,size_t bytes)=0;
  virtual bool write(uint32_t offset,const void* data,size_t bytes)=0;
  virtual bool erase(uint32_t offset)=0;
};
class EvidenceStore {
 public:
  static constexpr uint32_t kSector=4096,kMaxBlob=2304,kKeys=20;
  enum Type:uint16_t {Probe=1,Failure=2,Ble=3};
  struct Status {
    bool ready=false,foreign=false;
    uint32_t sectors=0,commits=0,errors=0,corrupt=0,uncommitted=0;
    uint64_t last_sequence=0;
  };
  bool begin(EvidenceFlash& flash,bool initialize_blank);
  bool put(Type type,unsigned slot,const void* data,size_t bytes);
  size_t length(Type type,unsigned slot) const;
  size_t get(Type type,unsigned slot,void* data,size_t bytes);
  const Status& status() const {return status_;}
 private:
  struct Header {
    uint32_t magic=0;
    uint16_t version=1,type=0;
    uint64_t sequence=0;
    uint32_t slot=0,bytes=0,crc=0,commit=0xFFFFFFFFU;
  };
  static_assert(sizeof(Header)==32,"flash ABI");
  struct Index {uint32_t offset=UINT32_MAX,bytes=0;uint64_t sequence=0;};
  static int key(Type type,unsigned slot);
  static uint32_t checksum(const Header& h,const void* data,size_t bytes);
  bool owner(const Header& h) const;
  bool valid(uint32_t offset,Header& h);
  bool protectedSector(uint32_t offset) const;
  EvidenceFlash* flash_=nullptr;
  Status status_{};
  Index index_[kKeys]{};
  uint32_t cursor_=2;
  uint8_t scratch_[kMaxBlob]{};
};
EvidenceStore& evidenceStore();
bool initializeEvidenceStore(); // explicit startup, never from LOGCAP
} // namespace m5can
