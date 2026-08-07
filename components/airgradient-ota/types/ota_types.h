/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_TYPES_H
#define AG_OTA_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>

// Outcome of an OTA operation. Returned by the writer/source primitives and
// by OtaUpdater::run(). Only Ok indicates an image was applied.
enum class OtaStatus : uint8_t {
  Ok,              // image downloaded, written, and boot partition set
  UpToDate,        // server returned 304 - current firmware is newest
  Declined,        // server declined to serve an image (e.g. 400/404)
  TransportError,  // connection / DNS / read failure, or truncated download
  ServerError,     // unexpected HTTP status or empty body
  FlashError,      // esp_ota_begin/write/end/set_boot_partition failure
  InvalidImage,    // image failed validation at finish()
  InvalidArgument, // null/empty request field or null dependency
  Aborted,         // intentional cancel: phone ABORT or product teardown (BLE push)
};

// Progress state emission points (see OtaUpdater::run()). A terminal state
// (Done / Skipped / Failed) is ALWAYS emitted:
//   Starting    - is BLE-push only: OtaBleService::run() emits it through the
//                 progress callback at the start edge (before begin()). It is never carried on
//                 the wire or emitted by the pull path.
//   Checking    - emitted once before source.open()
//   Downloading - emitted once immediately after writer.begin(), then again
//                 during the read/write loop (throttled to
//                 CONFIG_AG_OTA_PROGRESS_INTERVAL_MS)
//   Applying    - emitted once immediately before writer.finish()
//   Done        - terminal: image written and boot partition set (Ok)
//   Skipped     - terminal: no update applied (UpToDate / Declined)
//   Failed      - terminal: any error outcome
enum class OtaState : uint8_t {
  Idle,
  Starting,
  Checking,
  Downloading,
  Applying,
  Done,
  Skipped,
  Failed
};

struct OtaProgress {
  OtaState state;
  size_t bytes_written;
  size_t total_size; // 0 when unknown (e.g. cellular chunked)
  uint8_t percent;   // 0..100; 0 when total unknown
};

// Set once before run(); the callback fires synchronously on the run() task.
using OtaProgressCallback = std::function<void(const OtaProgress &)>;

// Caller-supplied, per-update inputs. The string fields need only be valid
// during construction of the source - the source copies the bounded fields it
// needs into internal fixed buffers (see WifiHttpOtaSource).
struct OtaRequest {
  const char *serial_number;    // e.g. "aabbccddeeff"
  const char *current_firmware; // e.g. "3.1.21"
  const char *http_domain;      // e.g. "hw.airgradient.com"
};

#endif // AG_OTA_TYPES_H
