/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_IMAGE_SOURCE_H
#define AG_OTA_IMAGE_SOURCE_H

#include <cstddef>
#include <cstdint>

#include "types/ota_types.h"

// Pull transport seam. Shared by all device-initiated HTTP transports
// (WiFi now, cellular later) and driven by OtaUpdater.
class OtaImageSource {
public:
  virtual ~OtaImageSource() = default;

  // Resolve availability and open the byte stream.
  //  Ok                -> an update is available; read() will yield bytes
  //  UpToDate          -> server returned 304
  //  Declined          -> server declined to serve an image (e.g. 400/404)
  //  errors            -> TransportError / ServerError
  // out_total_size must be non-null; set to the image size when known, 0
  // otherwise. Passing nullptr returns InvalidArgument.
  virtual OtaStatus open(size_t *out_total_size) = 0;

  // Read the next chunk into buf.
  //  > 0 -> bytes read
  //    0 -> end of image (EOF)
  //  < 0 -> error (including invalid args: buf == nullptr || buf_size == 0)
  virtual int read(uint8_t *buf, size_t buf_size) = 0;

  // Release transport resources. Idempotent. The orchestrator calls this
  // after EVERY open() - including UpToDate and error returns - so the
  // implementation must tolerate close() whether or not a stream was opened.
  virtual void close() = 0;
};

#endif // AG_OTA_IMAGE_SOURCE_H
