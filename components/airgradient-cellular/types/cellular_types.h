/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CELLULAR_TYPES_H
#define CELLULAR_TYPES_H

#include <cstddef>
#include <cstdint>

inline constexpr int32_t CELLULAR_TIMEOUT_USE_DRIVER_DEFAULT = -1;
inline constexpr int CELLULAR_SIGNAL_CSQ_INVALID = -1;
inline constexpr int CELLULAR_RSSI_DBM_INVALID = -32768;
inline constexpr uint16_t CELLULAR_HTTP_STATUS_INVALID = 0;

enum class CellularStatus : uint8_t {
  Ok,
  Failed,
  Error,
  Timeout,
  InvalidArgument,
  BufferTooSmall,
  Unsupported,
};

enum class CellularTechnology : uint8_t {
  Auto,
  TwoG,
  Lte,
};

enum class CellularRegistrationState : uint8_t {
  Unknown,
  NotRegistered,
  Searching,
  RegisteredHome,
  RegisteredRoaming,
  Denied,
  EmergencyOnly,
};

enum class OperatorAccessTech : int {
  Gsm = 0,
  Utran = 2,
  EUtranLte = 7,
  Unknown = -1,
};

struct OperatorInfo {
  uint32_t operator_id = 0;
  OperatorAccessTech access_tech = OperatorAccessTech::Unknown;
};

struct OperatorList {
  static constexpr size_t MAX_OPERATORS = 16;
  OperatorInfo operators[MAX_OPERATORS] = {};
  size_t count = 0;
};

struct MutableBuffer {
  uint8_t *data = nullptr;
  size_t size = 0;
};

struct MutableStringBuffer {
  char *data = nullptr;
  size_t size = 0;
};

struct CellularSignalQuality {
  int csq = CELLULAR_SIGNAL_CSQ_INVALID;
  int rssi_dbm = CELLULAR_RSSI_DBM_INVALID;
};

struct CellularNetworkRegistration {
  CellularRegistrationState state = CellularRegistrationState::Unknown;
  uint32_t operator_id = 0;
  bool is_packet_service_ready = false;
};

struct CellularRegistrationRequest {
  CellularTechnology technology = CellularTechnology::Lte;
  const char *apn = nullptr;
  uint32_t operation_timeout_ms = 90000;
  uint32_t scan_timeout_ms = 600000;
};

struct CellularHttpTimeouts {
  int32_t connection_timeout_seconds = CELLULAR_TIMEOUT_USE_DRIVER_DEFAULT;
  int32_t response_timeout_seconds = CELLULAR_TIMEOUT_USE_DRIVER_DEFAULT;
};

struct CellularHttpGetRequest {
  const char *url = nullptr;
  MutableBuffer response_body = {};
  CellularHttpTimeouts timeouts = {};
};

struct CellularHttpPostRequest {
  const char *url = nullptr;
  const char *body = nullptr;
  const char *content_type = nullptr;
  MutableBuffer response_body = {};
  CellularHttpTimeouts timeouts = {};
};

struct CellularHttpResponse {
  uint16_t status_code = CELLULAR_HTTP_STATUS_INVALID;
  size_t body_size = 0;
  size_t body_bytes_written = 0;
  bool is_body_truncated = false;
};

#endif // CELLULAR_TYPES_H
