#include "ui.h"

#include <M5Dial.h>

#include <cstring>

#include "app_config.h"

namespace m5can {
namespace {

uint16_t statusColor(bool fault, bool query_active, uint32_t fps) {
  if (fault) return TFT_RED;
  if (query_active) return TFT_WHITE;
  if (fps > 0) return TFT_GREEN;
  return TFT_LIGHTGREY;
}

const char* statusText(bool fault, bool query_active, uint32_t fps) {
  if (fault) return "CAN ERROR";
  if (query_active) return "QUERY";
  if (fps > 0) return "CAN LIVE";
  return "CAN IDLE";
}

[[maybe_unused]] const char* bleText(bool ready, bool connected, bool elm) {
  if (!ready) return "BLE ERROR";
  if (elm) return "BLE ELM";
  if (connected) return "BLE LINK";
  return "BLE WAIT";
}

uint16_t bleColor(bool ready, bool connected, bool elm) {
  if (!ready) return TFT_RED;
  if (elm) return TFT_GREEN;
  if (connected) return TFT_WHITE;
  return TFT_LIGHTGREY;
}

template <typename FontT>
bool updateLine(char* cache, size_t cache_size,
                const char* text, int32_t y, int32_t h,
                uint16_t color, const FontT* font) {
  if (!cache || cache_size == 0 || !text || !font) return false;
  if (std::strncmp(cache, text, cache_size) == 0) return false;

  M5Dial.Display.fillRect(8, y, 224, h, TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(color, TFT_BLACK);
  M5Dial.Display.drawString(text, 120, y + h / 2, font);

  std::strncpy(cache, text, cache_size - 1);
  cache[cache_size - 1] = '\0';
  return true;
}

}  // namespace

void Ui::begin() {
  auto config = M5.config();
  config.serial_baudrate = 0;
  config.clear_display = true;
  config.output_power = true;
  M5Dial.begin(config, true, false);
  M5Dial.Display.setRotation(0);
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);

  // Static label is drawn once. Dynamic fields below are updated only
  // when their rendered text actually changes.
  M5Dial.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
#if !defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  M5Dial.Display.drawString("CAN frame/s", 120, 121, &fonts::Font2);
#endif
}

void Ui::showFatal(const char* title, const char* detail) {
  // A full clear is acceptable for a one-shot fatal screen.
  M5Dial.Display.fillScreen(TFT_BLACK);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5Dial.Display.drawString(
      title ? title : "FATAL", 120, 95, &fonts::Font4);
  M5Dial.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Dial.Display.drawString(
      detail ? detail : "unknown", 120, 135, &fonts::Font2);
}

bool Ui::pollDemoToggle() {
  M5Dial.update();
  const auto touch=M5Dial.Touch.getDetail();
  if (!touch.wasPressed()) return false;
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  // Only the lower half changes the status page. It cannot toggle DEMO.
  if (touch.y>=130 && touch.y<=226 && touch.x>=22 && touch.x<=218) {
    show_ble_peer_page_=!show_ble_peer_page_;
    return false;
  }
#endif
  return demo_touch_.onPress(touch.x,touch.y,millis());
}

void Ui::update(const CanMonitor& can, const BleStats& ble_stats, bool ble_ready,
                bool ble_connected, bool ble_elm, bool demo_auto,
                bool demo_active
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
                , const BlePeersSnapshot* peers
#endif
                ) {
  const uint32_t now = millis();
  if (now - last_refresh_ms_ < cfg::kDisplayRefreshMs) return;
  last_refresh_ms_ = now;

  const CanStats stats = can.stats();
  if (last_rate_ms_ == 0) {
    last_rate_ms_ = now;
    last_rx_frames_ = stats.rx_frames;
    last_query_count_ = stats.query_count;
    last_tx_success_ = stats.tx_success;
  }

  const uint32_t elapsed = now - last_rate_ms_;
  if (elapsed >= 1000) {
    frames_per_second_ =
        static_cast<uint32_t>(
            (stats.rx_frames - last_rx_frames_) * 1000ULL / elapsed);
    queries_per_second_x10_ =
        static_cast<uint32_t>(
            (stats.query_count - last_query_count_) * 10000ULL / elapsed);
    tx_per_second_x10_ =
        static_cast<uint32_t>(
            (stats.tx_success - last_tx_success_) * 10000ULL / elapsed);

    last_rx_frames_ = stats.rx_frames;
    last_query_count_ = stats.query_count;
    last_tx_success_ = stats.tx_success;
    last_rate_ms_ = now;
  }

  const bool fault =
      can.faultLocked() || stats.state == TWAI_STATE_BUS_OFF;
  const uint32_t lease_ms = can.leaseRemainingMs();

  char fps[20]{};
  char rx_total[28]{};
  char diag_rate[48]{};
  char tx_state[32]{};
  char errors[40]{};

  snprintf(fps, sizeof(fps), "%lu",
           static_cast<unsigned long>(frames_per_second_));
  // Ambient CAN RX can increase even when no diagnostic PID reply
  // succeeds. Show both counters side-by-side for on-device diagnosis.
  snprintf(rx_total, sizeof(rx_total), "CAN RX %llu   OBD OK %llu",
           static_cast<unsigned long long>(stats.rx_frames),
           static_cast<unsigned long long>(stats.query_ok));
  snprintf(diag_rate, sizeof(diag_rate),
           "DIAG %lu.%lu/s  TX %lu.%lu/s",
           static_cast<unsigned long>(queries_per_second_x10_ / 10),
           static_cast<unsigned long>(queries_per_second_x10_ % 10),
           static_cast<unsigned long>(tx_per_second_x10_ / 10),
           static_cast<unsigned long>(tx_per_second_x10_ % 10));
  if (lease_ms) {
    snprintf(tx_state, sizeof(tx_state), "TX LEASE %lu.%lus",
             static_cast<unsigned long>(lease_ms / 1000),
             static_cast<unsigned long>((lease_ms % 1000) / 100));
  } else {
    snprintf(tx_state, sizeof(tx_state), "TX LOCKED");
  }
  snprintf(errors, sizeof(errors), "D%llu M%lu O%lu B%lu",
           static_cast<unsigned long long>(stats.app_queue_drops),
           static_cast<unsigned long>(stats.driver_rx_missed),
           static_cast<unsigned long>(stats.driver_rx_overrun),
           static_cast<unsigned long>(stats.driver_bus_error));

#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  char dual_ble_text[28]{};
  unsigned connected_peers=0;
  if (peers) for (const auto& p : peers->peers)
    connected_peers += p.connected ? 1U : 0U;
  if(ble_stats.last_disconnect_reason_known)
    snprintf(dual_ble_text,sizeof(dual_ble_text),"BLE %u/2 R%X",connected_peers,static_cast<unsigned>(ble_stats.last_disconnect_reason));
  else snprintf(dual_ble_text,sizeof(dual_ble_text),"BLE %u/2 R--",connected_peers);
  const char* ble = ble_ready ? dual_ble_text : "BLE ERROR";
#else
  char ble_status[32]{};
  if (ble_stats.last_disconnect_reason_known) {
    std::snprintf(ble_status, sizeof(ble_status), "%s C%lu D%lu R%02X",
                  bleText(ble_ready, ble_connected, ble_elm),
                  static_cast<unsigned long>(ble_stats.connect_count),
                  static_cast<unsigned long>(ble_stats.disconnect_count),
                  static_cast<unsigned>(ble_stats.last_disconnect_reason));
  } else {
    std::snprintf(ble_status, sizeof(ble_status), "%s C%lu D%lu R--",
                  bleText(ble_ready, ble_connected, ble_elm),
                  static_cast<unsigned long>(ble_stats.connect_count),
                  static_cast<unsigned long>(ble_stats.disconnect_count));
  }
  const char* ble = ble_status;
#endif
  const uint16_t ble_color = bleColor(ble_ready, ble_connected, ble_elm);
  if (std::strncmp(cached_ble_, ble, sizeof(cached_ble_)) != 0 ||
      cached_ble_color_ != ble_color) {
    cached_ble_[0] = '\0';
    updateLine(cached_ble_, sizeof(cached_ble_),
               ble, 3, 20, ble_color, &fonts::Font2);
    cached_ble_color_ = ble_color;
  }

  const char* status = demo_active ? "DEMO DRIVE" :
      statusText(fault, stats.query_active, frames_per_second_);
  const uint16_t status_color = demo_active ? TFT_WHITE :
      statusColor(fault, stats.query_active, frames_per_second_);
  if (std::strncmp(cached_status_, status, sizeof(cached_status_)) != 0 ||
      cached_status_color_ != status_color) {
    cached_status_[0] = '\0';
    updateLine(cached_status_, sizeof(cached_status_),
               status, 22, 27, status_color, &fonts::Font4);
    cached_status_color_ = status_color;
  }

  // Two clearly distinct concepts: the selected DEMO setting (AUTO/OFF)
  // and the actual response source (DEMO DRIVE/CAN LIVE/IDLE/ERROR).
  // Only the central highlighted soft key is touch sensitive.
  const char* mode_label = demo_auto ? "DEMO AUTO  TAP" : "DEMO OFF  TAP";
  const uint16_t mode_color = demo_auto ? TFT_GREEN : TFT_LIGHTGREY;
  if (std::strncmp(cached_demo_mode_,mode_label,sizeof(cached_demo_mode_))!=0 ||
      cached_demo_mode_color_!=mode_color) {
    M5Dial.Display.fillRect(35,50,170,21,TFT_BLACK);
    M5Dial.Display.fillRect(DemoTouchControl::kLeft,
                            DemoTouchControl::kTop,
                            DemoTouchControl::kRight-DemoTouchControl::kLeft,
                            DemoTouchControl::kBottom-DemoTouchControl::kTop,
                            mode_color);
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(TFT_BLACK,mode_color);
    M5Dial.Display.drawString(mode_label,120,60,&fonts::Font2);
    std::strncpy(cached_demo_mode_,mode_label,sizeof(cached_demo_mode_)-1);
    cached_demo_mode_[sizeof(cached_demo_mode_)-1]='\0';
    cached_demo_mode_color_=mode_color;
  }

  updateLine(cached_fps_, sizeof(cached_fps_),
             fps, 72, 36, TFT_WHITE, &fonts::Font4);
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  updateLine(cached_lower_label_,sizeof(cached_lower_label_),
             show_ble_peer_page_ ? "BLE PEERS (TAP CAN)" : "CAN STATS (TAP BLE)",
             109,23,TFT_LIGHTGREY,&fonts::Font2);
  if (peers && show_ble_peer_page_) {
    if (last_peer_rate_ms_ == 0) {
      last_peer_rate_ms_ = now;
      last_peers_ = *peers;
    }
    const uint32_t peer_elapsed=now-last_peer_rate_ms_;
    if (peer_elapsed >= 1000) {
      for (size_t i=0;i<2;++i) {
        const auto& p=peers->peers[i];
        const auto& prev=last_peers_.peers[i];
        const bool same_session=p.connected && prev.connected &&
            p.generation==prev.generation &&
            p.connection_id==prev.connection_id;
        peer_rx_bytes_per_sec_[i]=same_session && p.rx_bytes>=prev.rx_bytes
            ? static_cast<uint32_t>((p.rx_bytes-prev.rx_bytes)*1000ULL/peer_elapsed) : 0;
        peer_tx_bytes_per_sec_[i]=same_session && p.tx_bytes>=prev.tx_bytes
            ? static_cast<uint32_t>((p.tx_bytes-prev.tx_bytes)*1000ULL/peer_elapsed) : 0;
      }
      last_peers_=*peers;
      last_peer_rate_ms_=now;
    }
    char p0_rx[40]{},p0_tx[40]{},p1_rx[40]{},p1_tx[40]{};
    const auto render=[&](size_t i,char* incoming,size_t in_size,
                          char* outgoing,size_t out_size) {
      const auto& p=peers->peers[i];
      if (!p.connected) {
        snprintf(incoming,in_size,"P%u -- WAIT",static_cast<unsigned>(i+1));
        if (out_size) outgoing[0]='\0';
        return;
      }
      snprintf(incoming,in_size,"P%u %04X RX%lu/s",
               static_cast<unsigned>(i+1),static_cast<unsigned>(p.address_suffix),
               static_cast<unsigned long>(peer_rx_bytes_per_sec_[i]));
      snprintf(outgoing,out_size,"TX%lu/s N%lu",
               static_cast<unsigned long>(peer_tx_bytes_per_sec_[i]),
               static_cast<unsigned long>(p.notifications));
    };
    render(0,p0_rx,sizeof(p0_rx),p0_tx,sizeof(p0_tx));
    render(1,p1_rx,sizeof(p1_rx),p1_tx,sizeof(p1_tx));
    updateLine(cached_rx_,sizeof(cached_rx_),p0_rx,133,23,TFT_WHITE,&fonts::Font2);
    updateLine(cached_diag_,sizeof(cached_diag_),p0_tx,156,22,TFT_LIGHTGREY,&fonts::Font2);
    updateLine(cached_tx_,sizeof(cached_tx_),p1_rx,179,22,TFT_WHITE,&fonts::Font2);
    updateLine(cached_errors_,sizeof(cached_errors_),p1_tx,202,24,TFT_LIGHTGREY,&fonts::Font2);
  } else {
#endif
  updateLine(cached_rx_, sizeof(cached_rx_),
             rx_total, 133, 23, TFT_WHITE, &fonts::Font2);
  updateLine(cached_diag_, sizeof(cached_diag_),
             diag_rate, 156, 22, TFT_WHITE, &fonts::Font2);

  const uint16_t tx_color =
      lease_ms ? TFT_WHITE : TFT_LIGHTGREY;
  if (std::strncmp(cached_tx_, tx_state, sizeof(cached_tx_)) != 0 ||
      cached_tx_color_ != tx_color) {
    cached_tx_[0] = '\0';
    updateLine(cached_tx_, sizeof(cached_tx_),
               tx_state, 179, 22, tx_color, &fonts::Font2);
    cached_tx_color_ = tx_color;
  }

  const bool has_errors =
      stats.app_queue_drops || stats.driver_rx_missed ||
      stats.driver_rx_overrun || stats.driver_bus_error;
  const uint16_t error_color =
      has_errors ? TFT_RED : TFT_LIGHTGREY;
  if (std::strncmp(cached_errors_, errors, sizeof(cached_errors_)) != 0 ||
      cached_error_color_ != error_color) {
    cached_errors_[0] = '\0';
    updateLine(cached_errors_, sizeof(cached_errors_),
               errors, 202, 24, error_color, &fonts::Font2);
    cached_error_color_ = error_color;
  }
#if defined(M5CAN_DUAL_BLE_EXPERIMENTAL)
  }
#endif
}

}  // namespace m5can
