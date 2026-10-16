#include "raw_can_partition.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_partition.h>
#endif

namespace m5can {
RawCanJournal& rawCanJournal() {
  static RawCanJournal store;
  return store;
}
bool initializeRawCanJournal() {
#if defined(ARDUINO_ARCH_ESP32)
  class Partition final:public RawCanFlash {
   public:
    const esp_partition_t* part=nullptr;
    uint32_t size() const override {return part?part->size:0;}
    bool read(uint32_t offset,void* p,size_t n) override {
      return part&&esp_partition_read(part,offset,p,n)==ESP_OK;
    }
    bool write(uint32_t offset,const void* p,size_t n) override {
      return part&&esp_partition_write(part,offset,p,n)==ESP_OK;
    }
  };
  static Partition flash;
  flash.part=esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(0x40),"canlog");
  if(!flash.part || flash.part->encrypted)return false;
  // An existing OTA slot overlapping the proposed CANLOG region will cause
  // owner check to fail. Never erase it implicitly, including on reset.
  return rawCanJournal().begin(flash);
#else
  return false;
#endif
}
} // namespace m5can
