#pragma once
#if defined(M5CAN_PASSIVE_TX_ISOLATED)
#if M5CAN_PASSIVE_TX_ISOLATED != 1 || defined(M5CAN_CAN_TX_ENABLED) || defined(M5CAN_VEHICLE_TRIAL) || defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL) || defined(M5CAN_CAN_MODE_DRY_RUN) || defined(M5CAN_CAN_TX_HARD_DISABLED)
#error "Conflicting passive CAN policy"
#endif
#ifdef __cplusplus
#include <driver/twai.h>
#endif
// Force-included for every application TU, after the SDK declarations.
// An accidental reintroduction fails compilation, even in unreachable code.
#pragma GCC poison twai_transmit TWAI_MODE_NORMAL TWAI_MODE_NO_ACK twai_initiate_recovery
#endif
