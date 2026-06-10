/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_UPDATER_H
#define AG_OTA_UPDATER_H

#include "hal/ota_image_source.h"
#include "hal/ota_image_writer.h"
#include "types/ota_types.h"

// Blocking pull orchestrator. Owns the open -> begin -> read/write loop ->
// finish flow, progress throttling, and abort-on-error. It touches only the
// two HAL interfaces and knows nothing about the transport.
class OtaUpdater {
public:
  OtaUpdater(OtaImageSource &source, OtaImageWriter &writer);

  void set_on_progress(OtaProgressCallback cb);

  // open -> begin -> loop(read -> write, throttled progress) -> finish.
  // Aborts the writer on any read/write error. Always closes the source.
  //
  // Blocking, non-reentrant, not thread-safe: run one update per instance at
  // a time. Concurrent or reentrant calls are undefined.
  OtaStatus run();

private:
  void _emit_progress(OtaState state, size_t total_size);

  OtaImageSource &_source;
  OtaImageWriter &_writer;
  OtaProgressCallback _on_progress;
};

#endif // AG_OTA_UPDATER_H
