#pragma once

// This trace is intentionally impossible to compile into TX-capable firmware.
// It uses preallocated RAM only and is dumped over USB only when all BLE
// clients have disconnected and the CAN owner has no pending transaction.
#if defined(M5CAN_BLE_DRY_TRACE_BENCH)
#if !defined(M5CAN_DUAL_BLE_EXPERIMENTAL) || !defined(M5CAN_CAN_MODE_DRY_RUN)
#error "M5CAN_BLE_DRY_TRACE_BENCH requires DUAL_BLE and physical CAN TX compiled out"
#endif

#include <Arduino.h>
#include <esp_timer.h>
#include <cstdint>

namespace m5can {

enum class DryTraceKind : uint8_t {
  Connect=1, Disconnect=2, CanQueueAccepted=3, CanOwnerResult=4,
  BleQueueAccepted=5, BleQueueRejected=6, BleNotifyApiAccepted=7,
  BleNotifyApiRejected=8, StaleOwnerDiscard=9
};

struct DryTraceRecord {
  uint64_t monotonic_us=0;
  uint32_t token=0, generation=0, can_id=0, identity=0;
  uint16_t conn=0, bytes=0;
  uint8_t kind=0, status=0;
};
static_assert(sizeof(DryTraceRecord)<=40, "bench trace entry exceeds RAM budget");

class DryTraceJournal {
 public:
  static constexpr uint16_t kCapacity=128;
  // This sequence is a correlation token in RAM; not an OTA or vehicle ID.
  uint32_t nextToken() {
    portENTER_CRITICAL(&mux_);
    ++token_;
    if (!token_)++token_;  // reserve zero for non-query BLE messages
    const uint32_t result=token_;
    portEXIT_CRITICAL(&mux_);
    return result;
  }

  void add(DryTraceKind kind,uint16_t conn=0,uint32_t generation=0,
           uint32_t token=0,uint32_t can_id=0,uint32_t identity=0,
           uint16_t bytes=0,uint8_t status=0) {
    DryTraceRecord r{};
    r.monotonic_us=static_cast<uint64_t>(esp_timer_get_time());
    r.kind=static_cast<uint8_t>(kind);
    r.conn=conn;r.generation=generation;r.token=token;
    r.can_id=can_id;r.identity=identity;r.bytes=bytes;r.status=status;
    portENTER_CRITICAL(&mux_);
    if(count_==kCapacity) {
      head_=static_cast<uint16_t>((head_+1)%kCapacity);
      ++overwritten_;
    } else ++count_;
    const uint16_t target=static_cast<uint16_t>((head_+count_-1)%kCapacity);
    records_[target]=r;
    ++total_;
    portEXIT_CRITICAL(&mux_);
  }

  // Never call from BLE callback or while CAN/BLE traffic is active.
  // A "BLE_API_ACCEPTED" event means NimBLE accepted a queued notification;
  // it does NOT prove that the peer received the bytes over radio.
  void dump() {
    uint16_t count=0,head=0;
    uint32_t total=0,overwritten=0;
    portENTER_CRITICAL(&mux_);
    count=count_;head=head_;total=total_;overwritten=overwritten_;
    portEXIT_CRITICAL(&mux_);
    Serial.printf("#DRYTRACE,BEGIN,version=1,origin=TX_DISABLED_BENCH,capacity=%u,total=%lu,kept=%u,overwritten=%lu,wire_can_tx_proven=0,ble_peer_delivery_proven=0\n",
                  kCapacity,static_cast<unsigned long>(total),count,
                  static_cast<unsigned long>(overwritten));
    for(uint16_t i=0;i<count;++i) {
      DryTraceRecord r{};
      portENTER_CRITICAL(&mux_);
      r=records_[(head+i)%kCapacity];
      portEXIT_CRITICAL(&mux_);
      Serial.printf("#DRYTRACE,EVENT,index=%u,t_us=%llu,kind=%u,token=%lu,conn=%u,gen=%lu,can_id=%08lX,identity=%08lX,bytes=%u,status=%u\n",
                    i,static_cast<unsigned long long>(r.monotonic_us),r.kind,
                    static_cast<unsigned long>(r.token),r.conn,
                    static_cast<unsigned long>(r.generation),
                    static_cast<unsigned long>(r.can_id),
                    static_cast<unsigned long>(r.identity),r.bytes,r.status);
    }
    Serial.println("#DRYTRACE,END");
  }

 private:
  portMUX_TYPE mux_=portMUX_INITIALIZER_UNLOCKED;
  DryTraceRecord records_[kCapacity]{};
  uint16_t head_=0,count_=0;
  uint32_t token_=0,total_=0,overwritten_=0;
};

// Inline function-local singleton: no dynamically allocated buffers.
inline DryTraceJournal& dryTraceJournal() {
  static DryTraceJournal journal;
  return journal;
}

} // namespace m5can
#endif // M5CAN_BLE_DRY_TRACE_BENCH
