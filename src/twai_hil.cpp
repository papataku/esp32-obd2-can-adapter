#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)
#include "twai_hil.h"
#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include <esp_timer.h>
#include <freertos/queue.h>

namespace hil {
namespace {
QueueHandle_t rx_queue=nullptr;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
bool installed=false,started=false;
twai_mode_t mode=TWAI_MODE_LISTEN_ONLY;
twai_status_info_t injected{};   // fault counters set by the PC
uint32_t tx_fail_next=0,tx_count=0,rx_injected=0,rx_dropped=0;

int hexNibble(char c){
  if(c>='0'&&c<='9')return c-'0';
  if(c>='A'&&c<='F')return c-'A'+10;
  if(c>='a'&&c<='f')return c-'a'+10;
  return -1;
}
// "HIL RX <id-hex> <ext 0|1> <data-hex up to 16 chars>"
bool injectRx(const char* args){
  char* end=nullptr;
  const unsigned long id=std::strtoul(args,&end,16);
  if(end==args||*end!=' ')return false;
  const char* p=end+1;
  if((*p!='0'&&*p!='1')||p[1]!=' ')return false;
  const bool ext=*p=='1';p+=2;
  twai_message_t m{};
  m.identifier=uint32_t(id);m.extd=ext?1:0;
  size_t n=0;
  while(p[0]&&p[1]&&n<8){
    const int hi=hexNibble(p[0]),lo=hexNibble(p[1]);
    if(hi<0||lo<0)return false;
    m.data[n++]=uint8_t(hi<<4|lo);p+=2;
  }
  if(*p)return false;
  if((ext&&id>0x1FFFFFFFUL)||(!ext&&id>0x7FFUL))return false;
  m.data_length_code=uint8_t(n);
  if(!rx_queue||!started||xQueueSend(rx_queue,&m,0)!=pdTRUE){
    portENTER_CRITICAL(&mux);++rx_dropped;++injected.rx_missed_count;portEXIT_CRITICAL(&mux);
  }else{
    portENTER_CRITICAL(&mux);++rx_injected;portEXIT_CRITICAL(&mux);
  }
  return true;
}
void status(){
  portENTER_CRITICAL(&mux);
  const twai_status_info_t s=injected;
  const uint32_t tx=tx_count,rx=rx_injected,drop=rx_dropped,fail=tx_fail_next;
  const bool inst=installed,run=started;const twai_mode_t md=mode;
  portEXIT_CRITICAL(&mux);
  Serial.printf("#HIL_STATUS,twai_hw=0,installed=%u,started=%u,mode=%s,tx=%lu,"
    "rx_injected=%lu,rx_dropped=%lu,tx_fail_next=%lu,state=%u,bus_error=%lu,"
    "rx_missed=%lu,rx_overrun=%lu\n",inst?1U:0U,run?1U:0U,
    md==TWAI_MODE_NORMAL?"NORMAL":md==TWAI_MODE_NO_ACK?"NO_ACK":"LISTEN_ONLY",
    (unsigned long)tx,(unsigned long)rx,(unsigned long)drop,(unsigned long)fail,
    unsigned(s.state),(unsigned long)s.bus_error_count,
    (unsigned long)s.rx_missed_count,(unsigned long)s.rx_overrun_count);
}
} // namespace

esp_err_t driver_install(const twai_general_config_t* g,const twai_timing_config_t*,
                         const twai_filter_config_t*){
  if(installed||!g)return ESP_ERR_INVALID_STATE;
  if(!rx_queue){
    rx_queue=xQueueCreate(32,sizeof(twai_message_t));
    if(!rx_queue)return ESP_ERR_NO_MEM;
    Serial.println("#HIL,serial_hil=1,twai_hw=0,can_gpio=UNUSED");
  }
  xQueueReset(rx_queue);
  portENTER_CRITICAL(&mux);installed=true;started=false;mode=g->mode;portEXIT_CRITICAL(&mux);
  return ESP_OK;
}
esp_err_t driver_uninstall(){
  if(!installed||started)return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&mux);installed=false;portEXIT_CRITICAL(&mux);
  return ESP_OK;
}
esp_err_t start(){
  if(!installed||started)return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&mux);started=true;
  if(injected.state==TWAI_STATE_STOPPED)injected.state=TWAI_STATE_RUNNING;
  portEXIT_CRITICAL(&mux);
  return ESP_OK;
}
esp_err_t stop(){
  if(!started)return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&mux);started=false;portEXIT_CRITICAL(&mux);
  return ESP_OK;
}
// Never touches hardware: the frame is only reported to the PC.
esp_err_t transmit(const twai_message_t* m,TickType_t){
  if(!m||!started||mode!=TWAI_MODE_NORMAL)return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&mux);
  const bool fail=tx_fail_next>0;
  if(fail){--tx_fail_next;++injected.tx_failed_count;}else ++tx_count;
  portEXIT_CRITICAL(&mux);
  if(fail)return ESP_FAIL;
  char hex[17]{};
  const unsigned n=m->data_length_code<=8?m->data_length_code:8;
  for(unsigned i=0;i<n;++i)std::snprintf(hex+2*i,3,"%02X",m->data[i]);
  Serial.printf("#HIL_TX,t_us=%llu,id=%08lX,ext=%u,dlc=%u,data=%s\n",
    (unsigned long long)esp_timer_get_time(),(unsigned long)m->identifier,
    unsigned(m->extd),unsigned(m->data_length_code),hex);
  return ESP_OK;
}
esp_err_t receive(twai_message_t* m,TickType_t ticks){
  if(!m||!installed||!rx_queue)return ESP_ERR_INVALID_STATE;
  if(!started){vTaskDelay(ticks?ticks:1);return ESP_ERR_TIMEOUT;}
  return xQueueReceive(rx_queue,m,ticks)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
esp_err_t get_status_info(twai_status_info_t* info){
  if(!info||!installed)return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&mux);
  *info=injected;
  if(!started)info->state=TWAI_STATE_STOPPED;
  info->msgs_to_rx=rx_queue?uint32_t(uxQueueMessagesWaiting(rx_queue)):0;
  portEXIT_CRITICAL(&mux);
  return ESP_OK;
}

bool usbCommand(const char* line){
  if(std::strncmp(line,"HIL",3)||(line[3]&&line[3]!=' '))return false;
  const char* a=line[3]?line+4:"";
  if(!std::strncmp(a,"RX ",3)){
    // Silent on success: per-frame status lines would distort timing.
    if(!injectRx(a+3))Serial.println("#HIL_ERR,BAD_RX");
    return true;
  }
  if(!std::strncmp(a,"TXFAIL ",7)){
    portENTER_CRITICAL(&mux);tx_fail_next=std::strtoul(a+7,nullptr,10);portEXIT_CRITICAL(&mux);
  }else if(!std::strncmp(a,"BUSERR ",7)){
    portENTER_CRITICAL(&mux);injected.bus_error_count+=std::strtoul(a+7,nullptr,10);portEXIT_CRITICAL(&mux);
  }else if(!std::strncmp(a,"RXMISS ",7)){
    portENTER_CRITICAL(&mux);injected.rx_missed_count+=std::strtoul(a+7,nullptr,10);portEXIT_CRITICAL(&mux);
  }else if(!std::strcmp(a,"BUSOFF")){
    portENTER_CRITICAL(&mux);injected.state=TWAI_STATE_BUS_OFF;portEXIT_CRITICAL(&mux);
  }else if(!std::strcmp(a,"RESET")){
    // Clears injected faults only. CanMonitor's latched fault stays latched
    // (production behavior); a reboot is required to recover, as on a car.
    portENTER_CRITICAL(&mux);
    injected=twai_status_info_t{};injected.state=started?TWAI_STATE_RUNNING:TWAI_STATE_STOPPED;
    tx_fail_next=0;
    portEXIT_CRITICAL(&mux);
    if(rx_queue)xQueueReset(rx_queue);
  }else if(std::strcmp(a,"STATUS")&&*a){
    Serial.println("#HIL_ERR,UNKNOWN");return true;
  }
  status();
  return true;
}
} // namespace hil
#endif
