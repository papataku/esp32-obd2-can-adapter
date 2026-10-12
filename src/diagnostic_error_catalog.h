#pragma once
#include <cstdint>
#include "diagnostic_policy.h"

namespace m5can {
// Stable event category/code ABI for exported BLELOG records.
// 1xx: BLE link/GATT/queue; 2xx: actual CAN DiagnosticStatus+200;
// 3xx: grouped-PID coordinator decision; 4xx: app/ELM/session;
// 5xx: accumulated CAN health counter deltas; 9xx: journal health.
// A code identifies *where* failure was observed, not its root cause.
enum class DiagnosticError : uint16_t {
  BleNoClientSlot=101, BleRxQueueOverflow=102,
  BleNotifyBackpressure=103, BleNotifySendFailed=104,
  BlePeerGenerationStale=105, BleDiagnosticRingOverflow=106,
  BleInitFailure=107,
  CanNoData=201, CanBusy=202, CanLeaseRequired=203,
  CanInvalidRequest=204, CanRateLimited=205, CanDriverError=206,
  CanBusOff=207, CanModeError=208, CanIsoTpError=209,
  CanUnknownStatus=299,
  GroupRejected=301, GroupStopped=302, GroupPeerGone=303,
  ElmBusy=401, ElmInputOverflow=402, ElmInvalidCommand=403,
  AppPeerGone=404, AppTaskCreateFailed=405,
  CanQueryTimeout=501, CanFailedCounter=502,
  CanRxQueueDrop=503, CanDriverMissed=504,
  CanDriverOverrun=505, CanBusErrorCounter=506,
  CanArbitrationLoss=507, CanInitFailed=508,
  BleRxOverflowCounter=521, BleNotifyFailureCounter=522,
  BleSessionRejectCounter=523, BleNotifyRetryCounter=524,
  JournalStorageFailure=901
};
// Exhaustively translate the CAN owner's result status to journal codes.
// No implicit enum-order arithmetic; adding a new DiagnosticStatus must be
// accompanied by a catalog/test update to prevent silent misclassification.
inline uint16_t diagnosticStatusErrorCode(DiagnosticStatus status) {
  switch(status) {
    case DiagnosticStatus::Ok:return 0;
    case DiagnosticStatus::NoData:return 201;
    case DiagnosticStatus::Busy:return 202;
    case DiagnosticStatus::LeaseRequired:return 203;
    case DiagnosticStatus::InvalidRequest:return 204;
    case DiagnosticStatus::RateLimited:return 205;
    case DiagnosticStatus::DriverError:return 206;
    case DiagnosticStatus::BusOff:return 207;
    case DiagnosticStatus::ModeError:return 208;
    case DiagnosticStatus::IsoTpError:return 209;
  }
  return 299;
}
inline const char* diagnosticErrorName(uint16_t raw) {
  if(raw==0)return "NONE";
  switch(static_cast<DiagnosticError>(raw)) {
    case DiagnosticError::BleNoClientSlot:return "BLE_NO_SLOT";
    case DiagnosticError::BleRxQueueOverflow:return "BLE_RX_OVERFLOW";
    case DiagnosticError::BleNotifyBackpressure:return "BLE_NOTIFY_RETRY";
    case DiagnosticError::BleNotifySendFailed:return "BLE_NOTIFY_FAILURE";
    case DiagnosticError::BlePeerGenerationStale:return "BLE_STALE_GENERATION";
    case DiagnosticError::BleDiagnosticRingOverflow:return "BLE_DIAG_OVERFLOW";
    case DiagnosticError::BleInitFailure:return "BLE_INIT_FAILURE";
    case DiagnosticError::CanNoData:return "CAN_NO_DATA";
    case DiagnosticError::CanBusy:return "CAN_BUSY";
    case DiagnosticError::CanLeaseRequired:return "CAN_LEASE_DENIED";
    case DiagnosticError::CanInvalidRequest:return "CAN_INVALID_REQUEST";
    case DiagnosticError::CanRateLimited:return "CAN_RATE_LIMIT";
    case DiagnosticError::CanDriverError:return "CAN_DRIVER_FAILURE";
    case DiagnosticError::CanBusOff:return "CAN_BUS_OFF";
    case DiagnosticError::CanModeError:return "CAN_MODE_FAILURE";
    case DiagnosticError::CanIsoTpError:return "ISOTP_FAILURE";
    case DiagnosticError::CanUnknownStatus:return "CAN_UNKNOWN_STATUS";
    case DiagnosticError::GroupRejected:return "PID_GROUP_REJECTED";
    case DiagnosticError::GroupStopped:return "PID_GROUP_STOPPED";
    case DiagnosticError::GroupPeerGone:return "PID_GROUP_PEER_GONE";
    case DiagnosticError::ElmBusy:return "ELM_BUSY";
    case DiagnosticError::ElmInputOverflow:return "ELM_INPUT_OVERFLOW";
    case DiagnosticError::ElmInvalidCommand:return "ELM_INVALID";
    case DiagnosticError::AppPeerGone:return "APP_PEER_GONE";
    case DiagnosticError::AppTaskCreateFailed:return "APP_TASK_CREATE_FAILED";
    case DiagnosticError::CanQueryTimeout:return "CAN_TIMEOUT_COUNTER";
    case DiagnosticError::CanFailedCounter:return "CAN_FAILED_COUNTER";
    case DiagnosticError::CanRxQueueDrop:return "CAN_QUEUE_DROPS";
    case DiagnosticError::CanDriverMissed:return "CAN_RX_MISSED";
    case DiagnosticError::CanDriverOverrun:return "CAN_RX_OVERRUN";
    case DiagnosticError::CanBusErrorCounter:return "CAN_BUS_ERRORS";
    case DiagnosticError::CanArbitrationLoss:return "CAN_ARB_LOST";
    case DiagnosticError::CanInitFailed:return "CAN_INIT_FAILED";
    case DiagnosticError::BleRxOverflowCounter:return "BLE_RX_DROPS";
    case DiagnosticError::BleNotifyFailureCounter:return "BLE_TX_FAILED";
    case DiagnosticError::BleSessionRejectCounter:return "BLE_SESSION_REJECT";
    case DiagnosticError::BleNotifyRetryCounter:return "BLE_TX_BACKPRESSURE";
    case DiagnosticError::JournalStorageFailure:return "JOURNAL_NVS_FAILURE";
    default:return "UNKNOWN_CODE";
  }
}
} // namespace m5can
