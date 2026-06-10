/**
 * AirGradient — OTA BLE Protocol Constants
 *
 * CBOR key/op names and the frozen state/result wire constants used by the
 * BLE push OTA GATT service (OtaBleService). Mirrors go_ble_protocol.h.
 *
 * The wire values are STABLE and one-directional (enum -> wire): reordering
 * OtaState / OtaStatus must never change the protocol. The numeric
 * assignments are frozen once the phone app ships.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_BLE_PROTOCOL_H
#define AG_OTA_BLE_PROTOCOL_H

#include <cstdint>

#include "types/ota_types.h"

// ---------------------------------------------------------------------------
// CBOR keys
// ---------------------------------------------------------------------------

// Control characteristic (phone -> device): {"op":..., "total":..., "fw":...}
inline constexpr const char *OTA_BLE_KEY_OP = "op";
inline constexpr const char *OTA_BLE_KEY_TOTAL = "total";
inline constexpr const char *OTA_BLE_KEY_FW = "fw";

// Status characteristic (device -> phone): {"state":..., "result":...}
inline constexpr const char *OTA_BLE_KEY_STATE = "state";
inline constexpr const char *OTA_BLE_KEY_RESULT = "result";

// ---------------------------------------------------------------------------
// Control op values (value of OTA_BLE_KEY_OP)
// ---------------------------------------------------------------------------

inline constexpr const char *OTA_BLE_OP_START = "start";
inline constexpr const char *OTA_BLE_OP_END = "end";
inline constexpr const char *OTA_BLE_OP_ABORT = "abort";

// ---------------------------------------------------------------------------
// Frozen wire constants (NOT enum ordering — see file header)
// ---------------------------------------------------------------------------

// OtaState wire values.
inline constexpr uint8_t OTA_BLE_STATE_DOWNLOADING = 0x01;
inline constexpr uint8_t OTA_BLE_STATE_APPLYING = 0x02;
inline constexpr uint8_t OTA_BLE_STATE_DONE = 0x03;
inline constexpr uint8_t OTA_BLE_STATE_FAILED = 0x04;

// OtaStatus wire values.
inline constexpr uint8_t OTA_BLE_RESULT_OK = 0x00;
inline constexpr uint8_t OTA_BLE_RESULT_FLASH_ERROR = 0x01;
inline constexpr uint8_t OTA_BLE_RESULT_INVALID_IMAGE = 0x02;
inline constexpr uint8_t OTA_BLE_RESULT_TRANSPORT_ERROR = 0x03;
inline constexpr uint8_t OTA_BLE_RESULT_ABORTED = 0x04;
inline constexpr uint8_t OTA_BLE_RESULT_INVALID_ARGUMENT = 0x05;

namespace ota_ble_protocol {

// Map an OtaState to its frozen wire value. Only the states reachable on the
// BLE push path are mapped; anything else falls back to Failed.
inline uint8_t to_wire(OtaState state) {
  switch (state) {
  case OtaState::Downloading:
    return OTA_BLE_STATE_DOWNLOADING;
  case OtaState::Applying:
    return OTA_BLE_STATE_APPLYING;
  case OtaState::Done:
    return OTA_BLE_STATE_DONE;
  case OtaState::Failed:
    return OTA_BLE_STATE_FAILED;
  default:
    return OTA_BLE_STATE_FAILED;
  }
}

// Map an OtaStatus to its frozen wire value. Only the results reachable on the
// BLE push path are mapped; anything else falls back to TransportError.
inline uint8_t to_wire(OtaStatus status) {
  switch (status) {
  case OtaStatus::Ok:
    return OTA_BLE_RESULT_OK;
  case OtaStatus::FlashError:
    return OTA_BLE_RESULT_FLASH_ERROR;
  case OtaStatus::InvalidImage:
    return OTA_BLE_RESULT_INVALID_IMAGE;
  case OtaStatus::TransportError:
    return OTA_BLE_RESULT_TRANSPORT_ERROR;
  case OtaStatus::Aborted:
    return OTA_BLE_RESULT_ABORTED;
  case OtaStatus::InvalidArgument:
    return OTA_BLE_RESULT_INVALID_ARGUMENT;
  default:
    return OTA_BLE_RESULT_TRANSPORT_ERROR;
  }
}

} // namespace ota_ble_protocol

#endif // AG_OTA_BLE_PROTOCOL_H
