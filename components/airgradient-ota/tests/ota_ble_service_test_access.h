/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_BLE_SERVICE_TEST_ACCESS_H
#define AG_OTA_BLE_SERVICE_TEST_ACCESS_H

#include <cstdint>

#include "services/ota_ble_service.h"
#include "types/ota_types.h"

// Friend test helper: drives the worker steps directly (no real FreeRTOS task)
// and exposes internal state for assertions. Under TEST_HOST the notify/
// semaphore handshake is a no-op, so tests pump the steps in order instead of
// relying on _worker_loop().
class OtaBleServiceTestAccess {
public:
  explicit OtaBleServiceTestAccess(OtaBleService &svc) : _svc(svc) {}

  bool begin_step() { return _svc._begin_step(); }
  bool drain_one() { return _svc._drain_one(); }
  void finish_step() { _svc._finish_step(); }
  void terminate(OtaStatus status, bool send_notify = true) {
    _svc._terminate(status, send_notify);
  }

  // Internal state-machine value (OtaBleService::State) as a raw byte.
  uint8_t internal_state() const { return static_cast<uint8_t>(_svc._state); }

  // The abort reason a signalling context (Control ABORT / disconnect / Data
  // violation) recorded for the worker to consume — lets tests assert the
  // trigger -> OtaStatus mapping without running a real worker task.
  OtaStatus pending_terminal_status() const { return _svc._terminal_status; }
  bool pending_suppress_notify() const { return _svc._suppress_notify; }

private:
  OtaBleService &_svc;
};

#endif // AG_OTA_BLE_SERVICE_TEST_ACCESS_H
