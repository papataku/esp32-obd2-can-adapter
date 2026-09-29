#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

namespace m5can::cfg {

constexpr gpio_num_t kCanTxPin = GPIO_NUM_13;
constexpr gpio_num_t kCanRxPin = GPIO_NUM_15;

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kCanBitrate = 500000;
constexpr uint32_t kTwaiRxQueueLen = 256;
constexpr uint32_t kTwaiTxQueueLen = 0;
constexpr uint32_t kFrameQueueLen = 512;
constexpr uint32_t kDisplayRefreshMs = 250;
constexpr uint32_t kStatsEmitMs = 1000;
constexpr uint32_t kTwaiReceiveWaitMs = 10;

constexpr size_t kNativeMaxPayload = 128;
constexpr size_t kNativeMaxDecodedPacket = 160;
constexpr size_t kNativeMaxEncodedPacket = 192;
constexpr uint8_t kNativeProtocolVersion = 1;

constexpr const char* kFirmwareName = "M5CAN-Dial";
constexpr const char* kFirmwareVersion = "0.2.0-phase2";

}  // namespace m5can::cfg
