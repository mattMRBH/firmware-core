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

// Friend test helper: drives the run() steps directly (no real run() loop) and
// exposes internal state for assertions. Under TEST_HOST the RTOS semaphore is
// a no-op, so tests pump the steps in order: feed a START/Data/END through the
// real GATT callbacks, then call begin_step/finish_step/terminate explicitly.
class OtaBleServiceTestAccess {
public:
  explicit OtaBleServiceTestAccess(OtaBleService &svc) : _svc(svc) {}

  bool begin_step() { return _svc._begin_step(); }
  void finish_step() { _svc._finish_step(); }
  void terminate(OtaStatus status) { _svc._terminate(status); }

  // Internal state-machine value (OtaBleService::State) as a raw byte.
  uint8_t internal_state() const { return static_cast<uint8_t>(_svc._state); }

  // The abort reason a signalling context (Control ABORT / disconnect / Data
  // violation) latched for run() to consume — lets tests assert the
  // trigger -> OtaStatus mapping without running the blocking run() loop.
  OtaStatus pending_terminal_status() const { return _svc._terminal_status; }
  bool finish_pending() const { return _svc._pending.load() == OtaBleService::Cmd::Finish; }
  bool abort_pending() const { return _svc._pending.load() == OtaBleService::Cmd::Abort; }

private:
  OtaBleService &_svc;
};

#endif // AG_OTA_BLE_SERVICE_TEST_ACCESS_H
