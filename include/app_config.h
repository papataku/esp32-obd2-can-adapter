#pragma once

#include <Arduino.h>
#include <driver/gpio.h>

namespace m5can::cfg {

constexpr gpio_num_t kCanTxPin = GPIO_NUM_13;
constexpr gpio_num_t kCanRxPin = GPIO_NUM_15;

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kCanBitrate = 500000;
constexpr uint32_t kTwaiRxQueueLen = 256;
constexpr uint32_t kTwaiListenTxQueueLen = 0;
constexpr uint32_t kTwaiNormalTxQueueLen = 1;
constexpr uint32_t kFrameQueueLen = 512;
constexpr uint32_t kDisplayRefreshMs = 250;
constexpr uint32_t kStatsEmitMs = 1000;
constexpr uint32_t kTwaiReceiveWaitMs = 10;

constexpr uint32_t kTxLeaseDefaultMs = 3000;
constexpr uint32_t kTxLeaseMaxMs = 5000;
constexpr uint32_t kMinDiagnosticIntervalMs = 50;
constexpr uint64_t kPostResponseQuietUs = 10000;
constexpr uint64_t kIsoTpCfTimeoutUs = 100000;
constexpr uint32_t kResponsePendingExtensionMs = 500;
constexpr uint32_t kMaxResponsePendingWindowMs = 1000;

constexpr size_t kNativeMaxPayload = 128;
constexpr size_t kNativeMaxDecodedPacket = 160;
constexpr size_t kNativeMaxEncodedPacket = 192;
constexpr uint8_t kNativeProtocolVersion = 1;

constexpr const char* kFirmwareName = "M5CAN-Dial";
constexpr const char* kFirmwareVersion = "0.3.2-phase3c";

}  // namespace m5can::cfg
