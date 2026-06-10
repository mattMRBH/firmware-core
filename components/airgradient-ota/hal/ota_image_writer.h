/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_OTA_IMAGE_WRITER_H
#define AG_OTA_IMAGE_WRITER_H

#include <cstddef>
#include <cstdint>

#include "types/ota_types.h"

// Universal flash-write core. Every transport - pull or push - terminates at
// an OtaImageWriter. It knows nothing about how the bytes are delivered.
class OtaImageWriter {
public:
  virtual ~OtaImageWriter() = default;

  // Select the next OTA partition and open it for writing.
  // total_size == 0 means unknown (OTA_SIZE_UNKNOWN).
  virtual OtaStatus begin(size_t total_size) = 0;

  // Append a chunk to the open partition. Must be called between begin()
  // and finish(). len == 0 returns InvalidArgument.
  virtual OtaStatus write(const uint8_t *data, size_t len) = 0;

  // Validate the image and set it as the next boot partition.
  // Does NOT reboot.
  virtual OtaStatus finish() = 0;

  // Free the open handle without activating the image. Idempotent.
  virtual void abort() = 0;

  // Total bytes accepted by write() since begin().
  virtual size_t bytes_written() const = 0;
};

#endif // AG_OTA_IMAGE_WRITER_H
