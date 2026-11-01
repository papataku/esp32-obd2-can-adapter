// Real monitor + owner + extracted production GATT bodies. Hooks only in host IO.
static bool tracking=false,revoked=false,pending=false,touch_stop=false;
static unsigned target=0,events=0,after_revoke_tx=0;
unsigned pr70GateDepth=0;
static m5can::DiagnosticRequest queued{};
static bool has_queue=false;
void disconnected(uint16_t);
void manualCanStop();
namespace signal_v2 {
struct CanOwnerTestAccess {
  static void setup(m5can::CanMonitor& m){
    m.tx_gate_=xSemaphoreCreateMutex();
    m.query_queue_=reinterpret_cast<void*>(1);m.result_queue_=reinterpret_cast<void*>(2);
    m.frame_queue_=reinterpret_cast<void*>(3);
  }
  static void clearPending(m5can::CanMonitor& m){m.query_in_flight_=false;}
  static void execute(m5can::CanMonitor& m,const m5can::DiagnosticRequest& q){
    m5can::DiagnosticResult result{};m.performQuery(q,result);
  }
  static bool acquire(m5can::CanMonitor& m,uint32_t epoch){
#ifdef PR70_EPOCH_API
    return m.acquireLease(550,epoch);
#else
    (void)epoch;return m.acquireLease(550);
#endif
  }
  static esp_err_t transmit(m5can::CanMonitor& m,uint32_t epoch,bool fc){
    auto f=fc?m.makeFlowControlFrame(0x18DA01F1):m.makeDiagnosticRequestFrame(queued);
#ifdef PR70_EPOCH_API
    return m.transmitOwnedFrame(f,20,epoch);
#else
    (void)epoch;return m.transmitOwnedFrame(f,20);
#endif
  }
};
}
void pr70Boundary(const char*){
  if(!tracking)return;
  if(++events==target)pending=true;
  // A callback arriving during admission must wait for the same task mutex.
  if(pending && !pr70GateDepth){
    tracking=false;pending=false;
    if(touch_stop)manualCanStop();else disconnected(7);
    revoked=true;
  }
}
void s1TestOnCriticalExit(portMUX_TYPE*){pr70Boundary("critical exit");}
int pr70QueueSend(QueueHandle_t q,const void* data,TickType_t){
  pr70Boundary("before queue copy");
  if(q==reinterpret_cast<void*>(1)){
    queued=*static_cast<const m5can::DiagnosticRequest*>(data);has_queue=true;
  }
  pr70Boundary("after queue copy");return pdTRUE;
}
int pr70QueueReceive(QueueHandle_t,void*,TickType_t){return 0;}
esp_err_t twai_transmit(const twai_message_t*,TickType_t){
  // End the schedule sweep at the first driver admission.
  if(!pending)tracking=false;
  ++tx;if(revoked)++after_revoke_tx;return ESP_OK;
}
esp_err_t twai_get_status_info(twai_status_info_t*){pr70Boundary("driver status");return ESP_OK;}
esp_err_t twai_receive(twai_message_t*,TickType_t wait){
  pr70Boundary("driver receive");g_test_millis+=wait;return ESP_ERR_TIMEOUT;
}
