#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
#include "twai_hil.h"
#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <esp_timer.h>
#include <esp_system.h>

namespace hil {
namespace {
// A single lock linearizes lifecycle, RX admission and evidence publication.
// No queue, USB, allocation, delay or other blocking API is called under it.
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
constexpr unsigned MaxRx=256, MaxEvents=128;
twai_message_t rx[MaxRx]{};
unsigned capacity=0,head=0,count=0;
bool installed=false;
twai_mode_t mode=TWAI_MODE_LISTEN_ONLY;
twai_status_info_t state{};
uint32_t generation=0,tx_fail_next=0,boot_id=0;
struct Event { uint64_t us; uint32_t seq,gen; char detail[208]; };
Event events[MaxEvents]{};
unsigned event_head=0,event_count=0;
uint32_t sequence=0,lost=0;
bool transport_fault=false;
// Instrumentation only: the successful FC event and its matching CF admissions.
// This never grants admission, renews a lease, or changes ISO-TP deadlines.
uint64_t fc_us=0;
uint32_t fc_seq=0,fc_generation=0,fc_response=0;
bool fc_ext=false,fc_pending=false;

bool eventLocked(const char* detail){
  ++sequence;
  if(event_count==MaxEvents){++lost;transport_fault=true;++state.rx_missed_count;return false;}
  auto &e=events[(event_head+event_count++)%MaxEvents];
  e.us=esp_timer_get_time();e.seq=sequence;e.gen=generation;
  std::strncpy(e.detail,detail,sizeof(e.detail)-1);
  return true;
}
bool frameLocked(const char* kind,const twai_message_t& m,bool ok){
  char hex[17]{},detail[208]{};
  for(unsigned i=0;i<m.data_length_code && i<8;++i)std::snprintf(hex+2*i,3,"%02X",m.data[i]);
  std::snprintf(detail,sizeof(detail),"kind=%s,id=%08lX,ext=%u,dlc=%u,data=%s,ok=%u",
    kind,(unsigned long)m.identifier,unsigned(m.extd),unsigned(m.data_length_code),hex,ok?1U:0U);
  if(ok && !std::strcmp(kind,"rx") && fc_pending &&
     generation==fc_generation && m.identifier==fc_response &&
     bool(m.extd)==fc_ext && m.data_length_code && (m.data[0]>>4)==2){
    const size_t used=std::strlen(detail);
    std::snprintf(detail+used,sizeof(detail)-used,
      ",fc_seq=%lu,fc_t_us=%llu,fc_to_cf_us=%llu",
      (unsigned long)fc_seq,(unsigned long long)fc_us,
      (unsigned long long)(uint64_t(esp_timer_get_time())-fc_us));
  }
  return eventLocked(detail);
}
bool valid(const twai_message_t& m){
  return m.data_length_code<=8 && m.identifier<=(m.extd?0x1FFFFFFFUL:0x7FFUL);
}
// Admission and stop share the same linearization point. No parsed injection
// can straddle stop/drain/reinstall: it is either in that queue or rejected.
bool admitLocked(const twai_message_t& m,uint32_t target){
  const bool running=installed && state.state==TWAI_STATE_RUNNING;
  const bool accepted=running && target==generation && count<capacity && !transport_fault;
  if(accepted){rx[(head+count++)%capacity]=m;}
  else if(running && target==generation && count==capacity){++state.rx_missed_count;}
  frameLocked("rx",m,accepted);
  return accepted;
}
bool parseRx(const char* a,twai_message_t& m){
  char *end=nullptr;const unsigned long id=std::strtoul(a,&end,16);
  if(end==a||*end!=' '||id>0x1FFFFFFFUL)return false;
  const char *p=end+1;
  if((p[0]!='0'&&p[0]!='1')||p[1]!=' ')return false;
  m.identifier=id;m.extd=p[0]=='1';p+=2;
  unsigned n=0;
  while(*p){
    if(!p[1]||n==8)return false;
    auto nib=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;};
    int hi=nib(p[0]),lo=nib(p[1]);if(hi<0||lo<0)return false;
    m.data[n++]=uint8_t(hi*16+lo);p+=2;
  }
  m.data_length_code=n;return valid(m);
}
}
uint32_t currentGeneration(){
  portENTER_CRITICAL(&mux);const uint32_t g=generation;portEXIT_CRITICAL(&mux);return g;
}
void note(const char* format,...){
  char detail[208]{};va_list ap;va_start(ap,format);
  int n=vsnprintf(detail,sizeof(detail),format,ap);va_end(ap);
  portENTER_CRITICAL(&mux);
  if(n<0||unsigned(n)>=sizeof(detail)){transport_fault=true;++lost;++state.rx_missed_count;}
  else eventLocked(detail);
  portEXIT_CRITICAL(&mux);
}
// Only the USB task calls this. Partial writes are retained, never retried as
// duplicate records. A disconnected/backlogged host cannot block the CAN owner.
void poll(){
  static char pending[320]{};static size_t size=0,offset=0;
  for(unsigned records=0;records<8;++records){
    if(!size){
      Event e{};bool have=false;uint32_t dropped;
      portENTER_CRITICAL(&mux);
      dropped=lost;
      if(event_count){e=events[event_head];event_head=(event_head+1)%MaxEvents;--event_count;have=true;}
      portEXIT_CRITICAL(&mux);
      if(!have)return;
      size=std::snprintf(pending,sizeof(pending),"#HIL_EVENT,seq=%lu,t_us=%llu,gen=%lu,lost=%lu,boot=%08lX,%s\n",
        (unsigned long)e.seq,(unsigned long long)e.us,(unsigned long)e.gen,(unsigned long)dropped,(unsigned long)boot_id,e.detail);
    }
    const int room=Serial.availableForWrite();
    if(room<=0)return;
    const size_t n=Serial.write(reinterpret_cast<const uint8_t*>(pending)+offset,
      size-offset<size_t(room)?size-offset:size_t(room));
    offset+=n;
    if(offset==size){size=offset=0;}
    else return; // Retain partial records; do not spin on a backlogged host.
  }
}
esp_err_t driver_install(const twai_general_config_t* g,const twai_timing_config_t* t,const twai_filter_config_t* f){
  if(!g||!t||!f||!g->rx_queue_len||g->rx_queue_len>MaxRx||
     (g->mode!=TWAI_MODE_NORMAL&&g->mode!=TWAI_MODE_NO_ACK&&g->mode!=TWAI_MODE_LISTEN_ONLY))return ESP_ERR_INVALID_ARG;
  portENTER_CRITICAL(&mux);
  if(installed){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
  if(!boot_id)boot_id=esp_random();
  capacity=g->rx_queue_len;head=count=0;installed=true;mode=g->mode;
  state=twai_status_info_t{};state.state=TWAI_STATE_STOPPED;++generation;
  eventLocked("kind=install");portEXIT_CRITICAL(&mux);return ESP_OK;
}
esp_err_t driver_uninstall(){
  portENTER_CRITICAL(&mux);
  if(!installed||(state.state!=TWAI_STATE_STOPPED&&state.state!=TWAI_STATE_BUS_OFF)){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
  installed=false;head=count=0;eventLocked("kind=uninstall");
  portEXIT_CRITICAL(&mux);return ESP_OK;
}
esp_err_t start(){
  portENTER_CRITICAL(&mux);
  if(!installed||state.state!=TWAI_STATE_STOPPED||transport_fault){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
  head=count=0;state.state=TWAI_STATE_RUNNING;eventLocked("kind=start");
  portEXIT_CRITICAL(&mux);return ESP_OK;
}
esp_err_t stop(){
  portENTER_CRITICAL(&mux);
  if(!installed||state.state!=TWAI_STATE_RUNNING){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
  fc_pending=false;
  state.state=TWAI_STATE_STOPPED;eventLocked("kind=stop");
  portEXIT_CRITICAL(&mux);return ESP_OK;
}
esp_err_t transmit(const twai_message_t* m,TickType_t){
  if(!m||!valid(*m))return ESP_ERR_INVALID_ARG;
  portENTER_CRITICAL(&mux);
  if(!installed||state.state!=TWAI_STATE_RUNNING||mode==TWAI_MODE_LISTEN_ONLY||transport_fault){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
  const bool fail=tx_fail_next>0;
  if(fail){--tx_fail_next;++state.tx_failed_count;}
  const bool recorded=frameLocked("tx",*m,!fail);
  fc_pending=false;
  if(recorded && !fail && m->data_length_code>=3 && m->data[0]==0x30){
    fc_seq=sequence;fc_generation=generation;fc_ext=m->extd;
    fc_us=events[(event_head+event_count-1)%MaxEvents].us;
    fc_response=m->extd ? ((m->identifier&0x1FFF0000UL)|
      ((m->identifier&0xFF)<<8)|((m->identifier>>8)&0xFF)) : m->identifier+8;
    fc_pending=true;
  }
  portEXIT_CRITICAL(&mux);
  return fail||!recorded?ESP_FAIL:ESP_OK;
}
esp_err_t receive(twai_message_t* m,TickType_t ticks){
  if(!m)return ESP_ERR_INVALID_ARG;
  // Stopped queues remain readable, including a zero-tick late-response drain.
  for(TickType_t elapsed=0;;++elapsed){
    portENTER_CRITICAL(&mux);
    if(!installed){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
    if(count){*m=rx[head];head=(head+1)%capacity;--count;portEXIT_CRITICAL(&mux);return ESP_OK;}
    portEXIT_CRITICAL(&mux);
    if(elapsed>=ticks)return ESP_ERR_TIMEOUT;
    vTaskDelay(1);
  }
}
esp_err_t get_status_info(twai_status_info_t* info){
  if(!info)return ESP_ERR_INVALID_ARG;
  portENTER_CRITICAL(&mux);
  if(!installed){portEXIT_CRITICAL(&mux);return ESP_ERR_INVALID_STATE;}
  *info=state;info->msgs_to_rx=count;
  if(transport_fault)++info->rx_missed_count;
  portEXIT_CRITICAL(&mux);return ESP_OK;
}
bool usbCommand(const char* line){
  if(std::strncmp(line,"HIL",3)||(line[3]&&line[3]!=' '))return false;
  const char* a=line[3]?line+4:"";
  if(!std::strncmp(a,"RX ",3)){
    twai_message_t m{};char* end=nullptr;
    const unsigned long target=std::strtoul(a+3,&end,10);
    if(end==a+3||*end!=' '||!target||!parseRx(end+1,m)){note("kind=error,reason=BAD_RX");return true;}
#if defined(M5CAN_HIL_HOST_TEST)
    admissionHook(); // Controlled parse/admission interleaving in host regression.
#endif
    portENTER_CRITICAL(&mux);admitLocked(m,uint32_t(target));portEXIT_CRITICAL(&mux);return true;
  }
  // FENCE echoes a fresh host nonce after all preceding commands have been
  // parsed. It cannot reset a latched transport fault or driver evidence.
  if(!std::strncmp(a,"FENCE ",6)){note("kind=fence,nonce=%.64s",a+6);return true;}
  portENTER_CRITICAL(&mux);
  if(!std::strncmp(a,"TXFAIL ",7))tx_fail_next=std::strtoul(a+7,nullptr,10);
  else if(!std::strncmp(a,"BUSERR ",7))state.bus_error_count+=std::strtoul(a+7,nullptr,10);
  else if(!std::strncmp(a,"RXMISS ",7))state.rx_missed_count+=std::strtoul(a+7,nullptr,10);
  else if(!std::strcmp(a,"BUSOFF"))state.state=TWAI_STATE_BUS_OFF;
  else if(!std::strcmp(a,"OVERFLOW")){
    // Deterministic queue overflow while holding the lifecycle lock; the owner
    // cannot drain between injections. Every admitted/rejected frame is traced.
    twai_message_t m{};m.identifier=0x18DAF101;m.extd=1;
    const unsigned admitted=installed && state.state==TWAI_STATE_RUNNING?capacity-count:0;
    for(unsigned i=0;i<admitted;++i)rx[(head+count++)%capacity]=m;
    if(admitted)++state.rx_missed_count;
    char summary[80]{};std::snprintf(summary,sizeof(summary),"kind=overflow,accepted=%u,rejected=1",admitted);
    eventLocked(summary);
  }else if(std::strcmp(a,"STATUS")){
    eventLocked("kind=error,reason=UNKNOWN_OR_UNSAFE_RESET");portEXIT_CRITICAL(&mux);return true;
  }
  char detail[208]{};
  std::snprintf(detail,sizeof(detail),"kind=command,command=%s,state=%u,pending=%u,missed=%lu",a,unsigned(state.state),count,(unsigned long)state.rx_missed_count);
  eventLocked(detail);portEXIT_CRITICAL(&mux);return true;
}
}
#endif
